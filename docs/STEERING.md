# PTZ follow-control design

Goal: keep a detected machine centered and usefully framed, with slow, smooth,
non-hunting camera motion. Detections arrive at 1–5 Hz (DetectX on-camera, or
an off-camera detector during development).

## Research summary (2026-08-13)

Surveyed practice (Frigate's autotracker redesign, PTZOptics/Lumens operator
guidance, Axis's own autotracking apps, Roboflow's PTZ control writeup):

- Production trackers converge on **velocity control** (VAPIX
  `continuouspantiltmove`) driven by a PD loop on the normalized pixel error,
  with a **deadband** ("quiet zone") so a roughly-centered target produces no
  motion, plus **error smoothing** (EMA / one-euro filter) so detection jitter
  doesn't become motor jitter.
- **Speed-by-zoom**: pan/tilt gains must scale with current field of view —
  at 20x zoom the same pixel error is a far smaller angle. Without this the
  tracker oscillates when zoomed in. (VAPIX absolute/center commands handle
  this internally; velocity commands do not.)
- **Step-and-settle** (absolute moves at ~1 Hz) is simpler and very robust for
  slow subjects; the classic smoothing trick is to close only a *fraction* of
  the error each step (exponential convergence, no overshoot). Downsides vs
  velocity mode: visible micro-steps on fast subjects; fine for machinery.
- **Zoom policy**: target box height as fraction of frame height, corrected
  only outside a hysteresis band (e.g. keep h/H in [0.35, 0.65], aim 0.5),
  because zoom hunting is the most distracting failure mode. Fixed zoom is a
  legitimate default.
- **Loss handling**: hold position briefly → zoom out stepwise to reacquire →
  optionally return to a home preset. Never spin while blind.

## Our two modes (both implemented in `steering/ptz_tracker.py`)

### `step` mode — the v1 default (user proposal, validated by research)
Every second, command the camera **half-way toward centering** using VAPIX
`center=<x>,<y>` — we pass the *midpoint pixel* between frame center and the
target center, and the camera converts pixels to angles itself (correct at any
zoom, no FOV math on our side). Zoom: if the target's height fraction leaves
the hysteresis band, apply `areazoom` sized to close half the (log) zoom error.
Exponential convergence: ~2–3 s to settle on a new target, immune to detection
jitter, motion is a short glide per step.

### `velocity` mode — the smooth upgrade path
`continuouspantiltmove` at 5–8 Hz: PD on EMA-smoothed normalized error,
deadband 5–6%, slew-rate clamp, gains scaled by `1/zoom_magnification`
(speed-by-zoom), stop-on-loss. Use when step motion looks too mechanical
(faster targets, cinematic requirements).

## Detection sources (pluggable)

- `mqtt` — DetectX on the camera publishes boxes (production path).
- `rtsp` — pull the camera's RTSP stream locally, run a YOLO checkpoint on
  CPU (~2-3 Hz), feed the same controller. Used for the loaner-PTZ experiment
  (loan devices block outgoing traffic, so on-camera MQTT can't reach us) and
  for developing against cameras with no model installed.

## steerx ACAP (on-camera port) — v0.2.0

`acap/steerx/` is the native C port of the step controller. v0.2.0 is wired to
the Stage 3 12-class machinery taxonomy (follow priority, machinery first,
`person` last so equipment beats workers): excavator, bulldozer, wheel loader,
dump truck, truck, mobile crane, tower crane, pump truck, concrete mixer,
pile driver, roller, person. Label strings must match the deployed model's
labels.txt exactly. Carries the loaner-validated tuning (APPROACH_FRAC 0.33,
DEADBAND 0.07). Built artifact: `acap/steerx/build/SteerX_PTZ_Follow_0_2_0_aarch64.eap`.

Not yet deployed to a live PTZ: the P3408 test camera is a fixed dome, and on
Axis loan devices the VAPIX service-account credentials are rejected (401), so
steerx can't read DetectX or drive VAPIX there. It needs an **owned ARTPEC-9
PTZ** running DetectX-with-the-machinery-model + steerx together to prove the
fully on-camera detect->steer loop. Until then the proven path is DetectX
on-camera + `ptz_tracker.py` off-camera (see config.example.yaml).
Not-yet-ported vs the off-camera controller: EMA box-center smoothing and
velocity mode.

## Loaner experiment target (Axis Virtual Loan, ends 2026-08-14 21:00)

AXIS Q6358-LE via 195.60.68.14 — HTTP :12091, RTSP :32091, user VLTuser.
Only PTZ moves and stream reads; no config changes, no app installs.
