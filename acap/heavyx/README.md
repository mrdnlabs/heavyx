# HeavyX

**One ACAP for Axis ARTPEC-9 cameras: heavy-machinery detection + PTZ auto-follow.**

Single .eap install. The 12-class machinery model (yolov5m INT8) is baked in,
detections are burned into the video stream (boxes + labels + stats) by the
camera's overlay engine, and on PTZ cameras the app steers the camera to keep
the highest-priority machine centered and framed. Fixed cameras get detection
+ overlay with steering cleanly disabled.

Classes: excavator, dump truck, wheel loader, truck, person, bulldozer,
roller, mobile crane, tower crane, pump truck, concrete mixer, pile driver.

## Install
1. Camera web UI -> Apps -> enable "Allow unsigned apps" (re-disable after).
2. Install `HeavyX_<ver>_aarch64.eap`, start it.
3. Open the app (Apps -> HeavyX -> Open) for the live HUD: follow-zone,
   priority list, AOI editing on video, health lights, steering log.

Steering ships **disabled** — enable it in the TRACK tab (PTZ cameras only).
Requires AXIS OS 12.x on ARTPEC-9. On 2 GB devices, stop AXIS Object
Analytics first (DLPU/RAM contention).

## Follow policy
The TRACK tab's priority list is the policy: order = precedence, checkbox =
followability. `person` ships enabled at lowest priority (followed only when
no machinery is visible); uncheck it to never follow people. Detection and
overlays always cover all classes.

## Diagnostics
- `/local/heavyx/status` — model/overlay/ptz health + live detections
- `/local/heavyx/tracking.log` — per-second steering JSONL (size-capped)
- Camera events: "HeavyX: <label>" per class + "HeavyX: Tracking" state

## Build
`docker build -t heavyx-build . && docker cp` the .eap out (see ../..
/scripts). Model swap = replace `app/model/model.tflite` + `labels.txt`,
rebuild (quantization constants are extracted at build time).

## Lineage & licenses
- Fork of [DetectX](https://github.com/pandosme/DetectX) by Fred Juhlin (MIT
  — see app/LICENSE and NOTICE). Steering ported from this repo's steerx ACAP.
- Detection UI concept: "Signal HUD" design (claude design package).
- **Model weights are YOLOv5-derived (AGPL-3.0)** trained on public
  construction datasets — review AGPL obligations before commercial
  redistribution. Weights trained on customer data can replace them.
