# HeavyX — Heavy Machinery Detection + PTZ Auto-Follow

**One ACAP for Axis ARTPEC-9 cameras**: detects heavy construction machinery
(12 classes) on-camera, burns boxes/labels/stats into the video stream, and —
on PTZ cameras — steers the camera to keep the highest-priority machine
centered and framed.

```
┌───────────────── Axis ARTPEC-9 camera ─────────────────┐
│  HeavyX ACAP (single .eap)                             │
│    ├─ YOLOv5m INT8 machinery model (baked in, DLPU)    │
│    ├─ axoverlay: boxes + labels burned into the stream │
│    ├─ PTZ follow: margin zoom · edge-clip · priority   │
│    ├─ Signal-HUD web UI at /local/heavyx/              │
│    └─ outputs: MQTT · ONVIF events · crops · JSONL log │
└────────────────────────────────────────────────────────┘
```

Classes: excavator, dump truck, wheel loader, truck, person, bulldozer,
roller, mobile crane, tower crane, pump truck, concrete mixer, pile driver.

## Install

Grab `HeavyX_<ver>_aarch64.eap` from
[Releases](https://github.com/mrdnlabs/heavyx/releases) →
camera Apps page → enable "Allow unsigned apps" (re-disable after) →
install → start → **Open**. Or:

```bash
cd acap/heavyx && AXIS_USER=root AXIS_PASS=... ./install.sh <camera-ip>
```

Steering ships **disabled**; enable it in the HUD's TRACK tab (PTZ cameras
only — fixed cameras run detection + overlay with steering cleanly off).
Requires AXIS OS 12.x. On 2 GB devices stop AXIS Object Analytics first.

## Repo layout

```
acap/heavyx/     the product — DetectX-fork ACAP (model baked in, PTZ, HUD)
steering/        off-camera dev controller (ptz_tracker.py; velocity mode lives here)
scripts/         model training/export pipeline (WSL2 + docker, Axis recipe)
docs/            plan, hardware findings, steering design history
design_scope/    Signal-HUD design mockups (implemented in acap/heavyx)
legacy/steerx/   superseded standalone steering ACAP (poc-v0.4.0 provenance)
```

Local-only (gitignored, not in the repo): `models/` (training artifacts),
`third_party/axis-model-zoo` ([upstream](https://github.com/AxisCommunications/axis-model-zoo)),
`vendor/` (DetectX mirror from [pandosme/DetectX](https://github.com/pandosme/DetectX/releases)),
`datasets/` (in WSL: ~/machinery/datasets).

## Training pipeline (WSL2 + RTX GPU)

```bash
bash scripts/setup_wsl_env.sh        # micromamba py3.11 + patched yolov5 + TF (export env)
bash scripts/setup_train_env.sh      # modern-torch training env (see note below)
python scripts/prep_stage3.py ...    # build the machinery dataset
bash scripts/train_stage2.sh         # (parameterized: NAME/DATA/WEIGHTS/EPOCHS/BATCH)
bash scripts/export_stage3.sh        # INT8 TFLite per the Axis recipe
```

Model swap: copy the new `.tflite` + `labels.txt` into
`acap/heavyx/app/model/`, rebuild the .eap (quantization constants are
extracted at build). Runtime upload via the Model page is only safe for
retrains with identical quantization.

Note: training and export use **separate** WSL envs — the Axis export recipe
pins TF 2.13 (forcing old torch that crashes modern NVIDIA drivers); training
uses its own env with current torch.

## Licenses

Code: MIT (fork of [DetectX](https://github.com/pandosme/DetectX) by Fred
Juhlin — see `acap/heavyx/NOTICE`). **Model weights are YOLOv5-derived
(AGPL-3.0)**, trained on public construction datasets (some CC BY-NC) —
review obligations before commercial redistribution; the intended production
path is retraining on customer site footage.

## History

Built as a staged PoC: COCO deploy-path validation → small fine-tune →
MOCS-merged 12-class model → standalone steering ACAP (`legacy/steerx`,
releases `poc-v0.3.0`/`poc-v0.4.0`) → merged into HeavyX 1.0.x. Details in
[docs/PLAN.md](docs/PLAN.md) and [docs/STEERING.md](docs/STEERING.md).
