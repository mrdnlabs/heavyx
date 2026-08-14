#!/usr/bin/env python3
"""Steer an Axis PTZ to follow a detected object. See docs/STEERING.md.

Two control modes:
  step      one absolute half-way-to-center move per tick (default 1 Hz) via
            VAPIX `center` — camera does all pixel->angle math; zoom kept in a
            hysteresis band via `areazoom`. Robust, jitter-proof, v1 default.
  velocity  continuouspantiltmove at 5-8 Hz with PD control, deadband, EMA
            smoothing and speed-by-zoom gain scaling. Smoother for faster
            targets.

Two detection sources:
  mqtt      DetectX on-camera detections (production).
  rtsp      local YOLO (CPU) on the camera's RTSP stream (development /
            cameras without a model installed).

Usage:
    python ptz_tracker.py --config config.yaml [--dry-run]
Password can be supplied via env PTZ_PASSWORD instead of the config file.
"""
import argparse
import math
import os
import threading
import time

import requests
import yaml
from requests.auth import HTTPDigestAuth


# --------------------------------------------------------------------------
# VAPIX client
# --------------------------------------------------------------------------
class Vapix:
    def __init__(self, cfg: dict, dry_run: bool = False):
        host = cfg["host"]
        port = cfg.get("http_port", 80)
        self.base = f"http://{host}:{port}/axis-cgi/com/ptz.cgi"
        password = cfg.get("password") or os.environ.get("PTZ_PASSWORD", "")
        self.auth = HTTPDigestAuth(cfg["user"], password)
        self.camera = str(cfg.get("camera_number", 1))
        self.dry_run = dry_run
        self._last_vel = None

    def _get(self, params: dict):
        params = {**params, "camera": self.camera}
        if self.dry_run:
            print(f"[dry-run] ptz.cgi {params}")
            return None
        try:
            return requests.get(self.base, params=params, auth=self.auth,
                                timeout=3)
        except requests.RequestException as e:
            print(f"VAPIX error: {e}")
            return None

    def position(self) -> dict:
        r = self._get({"query": "position"})
        out = {}
        if r is not None and r.ok:
            for line in r.text.splitlines():
                if "=" in line:
                    k, v = line.split("=", 1)
                    try:
                        out[k.strip()] = float(v)
                    except ValueError:
                        pass
        return out

    def center_on(self, x: int, y: int, imgw: int, imgh: int) -> None:
        self._get({"center": f"{x},{y}",
                   "imagewidth": imgw, "imageheight": imgh})

    def areazoom(self, x: int, y: int, factor: float,
                 imgw: int, imgh: int) -> None:
        # factor > 1 zooms in by that multiple around (x, y)
        f = max(1, int(round(factor * 100)))
        self._get({"areazoom": f"{x},{y},{f}",
                   "imagewidth": imgw, "imageheight": imgh})

    def zoom_relative(self, delta: int) -> None:
        self._get({"rzoom": delta})

    def continuous(self, pan: float, tilt: float, zoom: float = 0.0) -> None:
        cmd = (round(pan, 1), round(tilt, 1), round(zoom))
        if cmd == self._last_vel:
            return
        self._last_vel = cmd
        params = {"continuouspantiltmove": f"{cmd[0]},{cmd[1]}"}
        if cmd[2]:
            params["continuouszoommove"] = str(cmd[2])
        self._get(params)

    def stop(self) -> None:
        self.continuous(0, 0, 0)


# --------------------------------------------------------------------------
# Detection sources — emit dicts {label, conf(0-1), cx, cy, w, h} normalized
# --------------------------------------------------------------------------
class LatestDetections:
    def __init__(self):
        self.lock = threading.Lock()
        self.items = []
        self.ts = 0.0

    def put(self, items) -> None:
        with self.lock:
            self.items = items
            self.ts = time.monotonic()

    def get(self, max_age: float):
        with self.lock:
            if time.monotonic() - self.ts > max_age:
                return []
            return list(self.items)


def start_mqtt_source(cfg: dict, sink: LatestDetections) -> None:
    import json
    import paho.mqtt.client as mqtt

    fw, fh = cfg["frame"]["width"], cfg["frame"]["height"]

    def on_message(_c, _u, msg):
        try:
            payload = json.loads(msg.payload)
        except json.JSONDecodeError:
            return
        items = payload if isinstance(payload, list) else payload.get(
            "detections", [payload])
        batch = []
        for d in items:
            if not isinstance(d, dict) or "label" not in d:
                continue
            batch.append(dict(
                label=d["label"], conf=d.get("c", 0) / 100.0,
                cx=(d["x"] + d["w"] / 2) / fw, cy=(d["y"] + d["h"] / 2) / fh,
                w=d["w"] / fw, h=d["h"] / fh))
        sink.put(batch)

    m = cfg["mqtt"]
    client = mqtt.Client()
    if m.get("username"):
        client.username_pw_set(m["username"], m.get("password", ""))
    client.on_message = on_message
    client.connect(m["host"], m.get("port", 1883))
    client.subscribe(m["topic"])
    client.loop_start()
    print(f"mqtt source: {m['host']}:{m.get('port',1883)} {m['topic']}")


def _load_yolo(repo: str, weights: str, device: str = "cpu"):
    """Load a yolov5 checkpoint and return (model, names, infer_fn).
    infer_fn(bgr_frame) -> list of normalized detection dicts."""
    import sys
    import cv2  # noqa: F401  (ensures cv2 present for callers)
    import numpy as np
    import torch

    sys.path.insert(0, os.path.expanduser(repo))
    from models.common import DetectMultiBackend
    from utils.augmentations import letterbox
    from utils.general import non_max_suppression, scale_boxes

    os.environ.setdefault("TORCH_FORCE_NO_WEIGHTS_ONLY_LOAD", "1")
    model = DetectMultiBackend(os.path.expanduser(weights),
                               device=torch.device(device))
    names = model.names

    def infer(frame, conf_thres: float):
        h0, w0 = frame.shape[:2]
        img = letterbox(frame, 640, stride=32, auto=False)[0]
        img = img.transpose((2, 0, 1))[::-1]
        img = np.ascontiguousarray(img)
        t = torch.from_numpy(img).float() / 255.0
        det = non_max_suppression(model(t[None]), conf_thres, 0.45)[0]
        out = []
        if len(det):
            det[:, :4] = scale_boxes((640, 640), det[:, :4], (h0, w0)).round()
            for *xyxy, conf, cls in det.tolist():
                x1, y1, x2, y2 = xyxy
                out.append(dict(
                    label=names[int(cls)], conf=float(conf),
                    cx=(x1 + x2) / 2 / w0, cy=(y1 + y2) / 2 / h0,
                    w=(x2 - x1) / w0, h=(y2 - y1) / h0))
        return out

    return model, names, infer


def start_snapshot_source(cfg: dict, sink: LatestDetections) -> None:
    """Poll the camera's VAPIX JPEG endpoint and run local YOLO. More robust
    than RTSP over a WAN/port-forward; ~1-2 Hz is plenty for step mode."""
    import cv2
    import numpy as np

    r = cfg["snapshot_source"]
    _, _, infer = _load_yolo(r["yolo_repo"], r["weights"])
    conf = r.get("conf", 0.35)
    url = r["url"]
    auth = HTTPDigestAuth(cfg["camera"]["user"],
                          cfg["camera"].get("password")
                          or os.environ.get("PTZ_PASSWORD", ""))

    def loop():
        print("snapshot source: polling", url)
        while True:
            try:
                resp = requests.get(url, auth=auth, timeout=5)
                arr = np.frombuffer(resp.content, np.uint8)
                frame = cv2.imdecode(arr, cv2.IMREAD_COLOR)
            except Exception as e:
                print("snapshot fetch error:", e)
                time.sleep(1)
                continue
            if frame is not None:
                sink.put(infer(frame, conf))
            time.sleep(r.get("interval_s", 0.5))

    threading.Thread(target=loop, daemon=True).start()


def start_rtsp_source(cfg: dict, sink: LatestDetections) -> None:
    """Local CPU YOLO on the camera's RTSP stream. Runs in a thread at
    whatever rate the CPU sustains (~2-3 Hz for yolov5n/s at 640)."""
    import sys

    import cv2
    import numpy as np
    import torch

    r = cfg["rtsp_source"]
    sys.path.insert(0, os.path.expanduser(r["yolo_repo"]))
    from models.common import DetectMultiBackend
    from utils.augmentations import letterbox
    from utils.general import non_max_suppression, scale_boxes

    os.environ.setdefault("TORCH_FORCE_NO_WEIGHTS_ONLY_LOAD", "1")
    model = DetectMultiBackend(os.path.expanduser(r["weights"]), device="cpu")
    names = model.names
    conf_thres = r.get("conf", 0.4)
    url = r["url"].replace("${PTZ_PASSWORD}",
                           os.environ.get("PTZ_PASSWORD", ""))

    def loop():
        cap = cv2.VideoCapture(url, cv2.CAP_FFMPEG)
        if not cap.isOpened():
            print("rtsp source: FAILED to open stream")
            return
        print("rtsp source: stream open")
        while True:
            # drain buffered frames so we always process the newest
            for _ in range(3):
                cap.grab()
            ok, frame = cap.retrieve()
            if not ok:
                time.sleep(0.5)
                cap.release()
                cap = cv2.VideoCapture(url, cv2.CAP_FFMPEG)
                continue
            h0, w0 = frame.shape[:2]
            img = letterbox(frame, 640, stride=32, auto=False)[0]
            img = img.transpose((2, 0, 1))[::-1]  # BGR->RGB, HWC->CHW
            img = np.ascontiguousarray(img)
            t = torch.from_numpy(img).float() / 255.0
            pred = model(t[None])
            det = non_max_suppression(pred, conf_thres, 0.45)[0]
            batch = []
            if len(det):
                det[:, :4] = scale_boxes((640, 640), det[:, :4],
                                         (h0, w0)).round()
                for *xyxy, conf, cls in det.tolist():
                    x1, y1, x2, y2 = xyxy
                    batch.append(dict(
                        label=names[int(cls)], conf=float(conf),
                        cx=(x1 + x2) / 2 / w0, cy=(y1 + y2) / 2 / h0,
                        w=(x2 - x1) / w0, h=(y2 - y1) / h0))
            sink.put(batch)

    threading.Thread(target=loop, daemon=True).start()


# --------------------------------------------------------------------------
# Controller
# --------------------------------------------------------------------------
class Tracker:
    def __init__(self, cfg: dict, cam: Vapix, sink: LatestDetections):
        self.cfg = cfg
        self.cam = cam
        self.sink = sink
        self.current_class = None
        self.current_since = 0.0
        self.ema = None  # smoothed (cx, cy, h)

    def pick_target(self):
        t = self.cfg["targeting"]
        cands = [d for d in self.sink.get(t["max_age_s"])
                 if d["label"] in t["classes"] and d["conf"] >= t["min_confidence"]]
        if not cands:
            return None
        now = time.monotonic()
        if (self.current_class
                and now - self.current_since < t["stickiness_s"]
                and any(d["label"] == self.current_class for d in cands)):
            cands = [d for d in cands if d["label"] == self.current_class]
        else:
            best = min(t["classes"].index(d["label"]) for d in cands)
            cands = [d for d in cands if t["classes"].index(d["label"]) == best]
        target = max(cands, key=lambda d: d["w"] * d["h"])
        if target["label"] != self.current_class:
            self.current_class = target["label"]
            self.current_since = now
            self.ema = None
            print(f"tracking: {target['label']} ({target['conf']:.2f})")
        return target

    def smooth(self, d, alpha: float):
        cur = (d["cx"], d["cy"], d["h"])
        self.ema = cur if self.ema is None else tuple(
            alpha * c + (1 - alpha) * e for c, e in zip(cur, self.ema))
        return self.ema

    # ---------------- step mode (half-way centering, 1 Hz) ----------------
    def run_step(self):
        s = self.cfg["step"]
        z = self.cfg["zoom"]
        loss = self.cfg["loss"]
        W, H = 1000, 1000  # virtual image coords; VAPIX scales via imagewidth/height
        period = s.get("period_s", 1.0)
        frac = s.get("approach_fraction", 0.5)
        last_seen = 0.0
        zoomed_out = False

        while True:
            t0 = time.monotonic()
            d = self.pick_target()
            if d:
                last_seen = t0
                zoomed_out = False
                cx, cy, h = self.smooth(d, s.get("ema_alpha", 0.6))
                ex, ey = cx - 0.5, cy - 0.5
                if math.hypot(ex, ey) > s.get("deadband", 0.05):
                    # command the camera to center the point part-way toward
                    # the target: exactly closes `frac` of the angular error
                    px = int((0.5 + frac * ex) * W)
                    py = int((0.5 + frac * ey) * H)
                    self.cam.center_on(px, py, W, H)
                elif z["enabled"]:
                    # only touch zoom when roughly centered
                    lo, hi = z["band"]
                    if h < lo or h > hi:
                        want = z["target_height_frac"] / max(h, 1e-3)
                        factor = math.sqrt(want)  # half the error in log-zoom
                        factor = max(z.get("min_step", 0.7),
                                     min(z.get("max_step", 1.6), factor))
                        self.cam.areazoom(int(cx * W), int(cy * H), factor, W, H)
            else:
                gone = t0 - last_seen if last_seen else 0.0
                if (z["enabled"] and last_seen and not zoomed_out
                        and gone > loss["zoom_out_after_s"]):
                    print("target lost: zooming out to reacquire")
                    self.cam.zoom_relative(-z.get("reacquire_step", 2000))
                    zoomed_out = True
            time.sleep(max(0.0, period - (time.monotonic() - t0)))

    # ---------------- velocity mode (PD at 5-8 Hz) ----------------
    def run_velocity(self):
        c = self.cfg["velocity"]
        z = self.cfg["zoom"]
        loss = self.cfg["loss"]
        period = 1.0 / c.get("rate_hz", 6)
        last_seen = 0.0
        moving = False
        prev = None

        while True:
            t0 = time.monotonic()
            d = self.pick_target()
            if d:
                last_seen = t0
                cx, cy, h = self.smooth(d, c.get("ema_alpha", 0.4))
                ex, ey = cx - 0.5, cy - 0.5
                dex = dey = 0.0
                if prev:
                    dex, dey = (ex - prev[0]) / period, (ey - prev[1]) / period
                prev = (ex, ey)
                # speed-by-zoom: scale gains down as we zoom in
                mag = max(1.0, self.cam.position().get("zoom", 1) / 1000.0) \
                    if c.get("speed_by_zoom", True) else 1.0
                def axis(e, de, gain, dgain):
                    if abs(e) < c.get("deadband", 0.055):
                        return 0.0
                    v = (gain * e + dgain * de) / mag
                    return max(-c["max_speed"], min(c["max_speed"], v))
                pan = axis(ex, dex, c["pan_gain"], c.get("pan_dgain", 8))
                tilt = -axis(ey, dey, c["tilt_gain"], c.get("tilt_dgain", 6))
                if self.cfg.get("invert_tilt"):
                    tilt = -tilt
                self.cam.continuous(pan, tilt, 0.0)
                moving = True
            else:
                gone = t0 - last_seen if last_seen else 0.0
                if moving and gone > loss["stop_after_s"]:
                    self.cam.stop()
                    moving = False
                    prev = None
                    self.current_class = None
                    print("target lost: stopped")
            time.sleep(max(0.0, period - (time.monotonic() - t0)))


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--config", default="config.yaml")
    ap.add_argument("--mode", choices=("step", "velocity"), default=None)
    ap.add_argument("--source", choices=("mqtt", "rtsp", "snapshot"),
                    default=None)
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()
    with open(args.config, encoding="utf-8") as f:
        cfg = yaml.safe_load(f)

    cam = Vapix(cfg["camera"], dry_run=args.dry_run)
    sink = LatestDetections()
    source = args.source or cfg.get("source", "mqtt")
    if source == "mqtt":
        start_mqtt_source(cfg, sink)
    elif source == "snapshot":
        start_snapshot_source(cfg, sink)
    else:
        start_rtsp_source(cfg, sink)

    tracker = Tracker(cfg, cam, sink)
    mode = args.mode or cfg.get("mode", "step")
    print(f"mode={mode} source={source}")
    try:
        (tracker.run_step if mode == "step" else tracker.run_velocity)()
    except KeyboardInterrupt:
        pass
    finally:
        cam.stop()


if __name__ == "__main__":
    main()
