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

## steerx v0.4.0 additions (validated live on P3408)

- **Margin-based zoom policy** replaces target-height zoom: the whole object
  must fit inside the center `MarginPercent` (default 33%) of the frame on
  BOTH axes. Fixes the "centered on the left half of the bulldozer" failure —
  a tightly-zoomed partial box lies about the object's center.
- **Edge-touch rule**: if the RAW box touches a frame edge (±3 px), the object
  is clipped -> immediate zoom-out step before any centering.
- **EMA smoothing** (α 0.35) of box center AND size, ported from the
  off-camera controller.
- **Runtime parameters** via axparameter (camera Apps page settings dialog /
  `param.cgi` group `root.Steerx`): `MarginPercent` (10-90), `OverlaySlot`
  (#D slot 1-16 for burned-in stats, 0=off), `LogMaxKB` (0=off).
- **Burned-in stats overlay**: sets dynamic text `#D<slot>` via
  `dynamicoverlay.cgi?action=settext`. The #D token must exist in an overlay:
  on OS 12 create it with the JSON API
  (`POST /axis-cgi/dynamicoverlay/dynamicoverlay.cgi`, method `addText`,
  `"text":"#D1"`, position e.g. `bottomLeft` — the legacy
  `Image.I0.Text.*` params are gone). Text: `SteerX: <label> <conf>% size <n>`
  plus `CLIPPED` when edge-touching.
- **Tracking log**: JSONL, one line/tick (raw box, EMA box, error, maxdim vs
  margin, edge flag, issued command), size-capped with one rotation,
  downloadable at `/local/steerx/tracking.log` (linked from the app UI).
- Burned-in *boxes* deliberately deferred to the DetectX-fork combined app:
  steerx polls at 1 Hz, so boxes would lag visibly; the fork can draw at
  inference rate via axoverlay.

## steerx ACAP (on-camera port) — v0.3.0

`acap/steerx/` is the native C port of the step controller, wired to the
Stage 3 12-class machinery taxonomy (follow priority, machinery first, `person`
last so equipment beats workers): excavator, bulldozer, wheel loader, dump
truck, truck, mobile crane, tower crane, pump truck, concrete mixer, pile
driver, roller, person. Label strings must match the deployed model's
labels.txt exactly. Carries the loaner-validated tuning (APPROACH_FRAC 0.33,
DEADBAND 0.07). Built artifact:
`acap/steerx/build/SteerX_PTZ_Follow_0_3_0_aarch64.eap`.

**Web UI (v0.3.0):** `settingPage` in the manifest adds an "Open" button in the
camera's Apps list, serving `app/html/index.html` at `/local/steerx/`. The page
uses the browser's authenticated session to live-poll DetectX detections and PTZ
position, showing which target SteerX is tracking, the follow-priority list, and
the control tuning. (The page mirrors the follow logic; steerx itself has no HTTP
endpoint yet.)

**Local VAPIX auth — SOLVED (v0.3.0), was the on-camera blocker:** service-account
credentials from `com.axis.HTTPConf1.VAPIXServiceAccounts1.GetCredentials` are
only valid on the virtual host **`127.0.0.12`** (NOT `127.0.0.1`) and must be
sent with **HTTP Basic** (NOT Digest — Digest returns 401). Verified live on the
loaner Q6355: steerx authenticates, reads `/local/detectx/*`, and reaches
`/axis-cgi/*`. This was previously misdiagnosed as a loan-device restriction — it
was the loopback address + auth scheme. Ref: `C:\_acap` guides
(vapix-local-auth-from-acap.md, acap-manifest-gotchas.md) and the official
acap-native-sdk-examples/vapix example.

**Build hygiene (from `C:\_acap` guides):** Dockerfile chmods files to 644/755
(WSL `/mnt/c` reports 777, which silently makes the installer skip the web-UI
proxy rules); `.dockerignore` excludes `*.eap` (acap-build merges manifest fields
from stale EAPs); build with `--no-cache` (WSL BuildKit stale-layer bug).

Remaining to fully prove on-camera: a live detect->steer run on an **owned**
ARTPEC-9 PTZ with the machinery model loaded and machinery actually in view (the
loaner runs stock COCO in a camera warehouse — steerx authenticates and idles
correctly with no machinery to follow). Steering *motion* itself is already
proven via the off-camera `ptz_tracker.py` (see the follow GIF). Not-yet-ported
vs the off-camera controller: EMA box-center smoothing and velocity mode.

## Loaner experiment target (Axis Virtual Loan, ends 2026-08-14 21:00)

AXIS Q6358-LE via 195.60.68.14 — HTTP :12091, RTSP :32091, user VLTuser.
Only PTZ moves and stream reads; no config changes, no app installs.
