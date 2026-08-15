# CLAUDE.md

Guidance for Claude Code when working in this directory.

## Project Overview

HeavyX is an ACAP (Axis Camera Application Platform) app for ARTPEC-9 cameras:
on-camera heavy-machinery detection (12-class YOLOv5m INT8, baked in), boxes/
labels/stats burned into the video stream via axoverlay, and PTZ auto-follow
steering. Fork of DetectX 4.1.1 by Fred Juhlin (MIT — see NOTICE); steering
ported from this repo's legacy/steerx.

## Build

```bash
docker build -t heavyx-build .    # axisecp/acap-native-sdk:12.8.0-aarch64-ubuntu24.04
# extract: docker create + docker cp /opt/app/HeavyX_<ver>_aarch64.eap
```
Always build with `--no-cache` on WSL2 (BuildKit stale-layer bug). The
Dockerfile chmods 644/755 (WSL 777 breaks web-UI registration) and runs
`extract_model_params.py` on `app/model/model.tflite` — INT8 quantization
constants are COMPILE-TIME (`model_params.h`); a runtime-uploaded model with
different quantization silently decodes garbage.

Model swap = replace `app/model/model.tflite` + `app/model/labels.txt`,
rebuild. Trained via the repo-root `scripts/` pipeline.

## Architecture (fork = upstream DetectX + three modules)

- `app/main.c` — GMainLoop; `ImageProcess()` idle-source inference loop
  (capture → `Model_Inference` → filters → hook). HeavyX hook at the end:
  `OVERLAY_Update()` + `PTZ_Feed()` before `Output()`. Error paths reschedule
  after 2 s (fork fix — upstream died permanently).
- `app/PTZ.c` (fork) — 1 Hz steering tick on the main loop. Settings-driven
  priority (`ptz.priority`: order = precedence, inclusion = followable),
  margin zoom + edge-touch rule, EMA (`ptz.emaAlpha`), sticky/loss handling.
  VAPIX via `ACAP_VAPIX_Get()` (D-Bus service account, 127.0.0.12 + Basic).
  JSONL tick log at `localdata/tracking.log`, served by the `tracking`
  FastCGI endpoint. PTZ support probe: `Properties.PTZ.PTZ=yes` AND
  `DigitalPTZ=no` (fixed domes report digital PTZ).
- `app/OVERLAY.c` (fork) — axoverlay burn-in: 4-bit palette overlay for
  boxes (class-group colors) + ARGB32 overlay for label text and a stats
  footer. Callbacks run on the main loop. Non-fatal if overlay engine absent.
- `app/MAP.h` (fork) — model→video coordinate mapping (stretch/crop/
  letterbox), mirrored in JS in `html/index.html`. Three places must stay in
  sync: MAP.h, Model.c geometry, index.html mapModelToPct.
- Upstream modules unchanged in role: `ACAP.c` (wrapper: HTTP FastCGI on its
  own pthread, settings persistence with KEY-MUST-EXIST merge, status,
  events, VAPIX), `Model.c` (larod), `Video.c` (VDO YUV), `Output.c` (MQTT/
  events/crops), `MQTT.c`, `CERTS.c`.

## Critical conventions

- New settings keys MUST be pre-declared in `app/settings/settings.json` —
  the POST merge silently drops unknown keys.
- New HTTP endpoints need BOTH `ACAP_HTTP_Node()` registration AND a
  manifest `httpConfig` entry.
- Threading: detection, timers, and overlay callbacks share the GLib main
  thread (no locks needed among them); HTTP handlers run on a separate
  pthread — never mutate steering/overlay state from them.
- Never retain the detections cJSON pointer past the hook (main.c deletes
  it); deep-copy (see PTZ_Feed / OVERLAY_Update).
- `runMode` is `respawn` — the model-upload endpoint restarts by SIGTERM.

## Web UI

`html/index.html` — the Signal-HUD single-page UI (vanilla JS, self-hosted
fonts in `html/fonts/`, no CDNs — air-gap safe). Tabs: DETECT / TRACK /
OUTPUT; drag-the-corners follow zone; on-video AOI editing; health lights
read `/local/heavyx/status` (`model.state`, `overlay.created`, `ptz.*`).
`html/classic.html` — preserved upstream DetectX UI (shares the other pages:
model/mqtt/advanced/cropping/crops/about/certificate).

## Endpoints (all admin, FastCGI, at /local/heavyx/)

app · settings · status · model · mqtt · certs · crops · sd_download ·
sd_clear · tracking (JSONL steering log; `?old=1` for the rotation)

## Debug

- `journalctl -f -u heavyx` on-camera (or systemlog.cgi?appname=heavyx).
- `/local/heavyx/status`: `model.state` and `overlay.created` are the real
  health (the Apps list "Running" lies when the model failed to load).
- DLPU/RAM contention: on 2 GB devices stop AXIS Object Analytics first
  (`objectanalytics`); symptom is model-load failure with app "Running".
