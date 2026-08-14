# Heavy Machinery Auto-Tracking for Axis PTZ

Detect heavy construction machinery (excavators, dozers, loaders, dump trucks, …)
on an Axis PTZ camera and auto-steer the camera to follow it.

**Architecture (PoC):**

```
┌─────────────────────────── Axis PTZ (ARTPEC-9, e.g. Q6355-LE) ──┐
│  DetectX ACAP  ──runs──▶  custom YOLOv5n .tflite (INT8)         │
│       │                                                          │
│       └── MQTT detections ──▶ broker ──▶ steering/ptz_tracker.py │
│                                              │                   │
│  VAPIX PTZ API  ◀── continuous-move commands ┘                   │
└──────────────────────────────────────────────────────────────────┘
```

Detection runs on-camera via [DetectX](https://github.com/pandosme/DetectX)
(open-source ACAP that loads a custom YOLOv5 `.tflite` + `labels.txt` and emits
detections over MQTT). Steering starts as an off-camera Python controller
(`steering/ptz_tracker.py`) driving the camera over VAPIX — fastest to iterate.
Once the control loop is tuned it gets ported into a native ACAP so everything
runs on-camera.

## Status (2026-08-13)

- Test camera: **AXIS P3408-VE fixed dome** at 192.168.1.141 (ARTPEC-9 DLPU,
  AXIS OS 12.10.68). Stage 0 deployed and verified: DetectX 4.1.1 running the
  Axis pretrained COCO yolov5n at ~45 ms/frame on the DLPU.
- Steering waits on PTZ hardware (Q6355-LE recommended). Customer's current
  P5655-E (ARTPEC-7, no DLPU) can't run YOLO.
- Training machine: local **RTX 4070 Laptop, 8 GB VRAM** (WSL2; needed
  `gpuSupport=true` in `~/.wslconfig`). ~99 GB free disk.
- See [docs/PLAN.md](docs/PLAN.md) for the staged plan and verified toolchain facts.

## Staged plan (summary)

| Stage | Cost | What it proves |
|-------|------|----------------|
| 0 | $0 | Stock COCO yolov5n → Axis INT8 export → DetectX on camera. Validates the whole convert–quantize–deploy path. Steering loop built in parallel against COCO `person`/`truck`. |
| 1 | ~$0 | Tiny fine-tune (10–20 epochs) on the small Roboflow construction set. Validates training→camera path, class taxonomy, PTQ calibration, float-vs-INT8 eval. |
| 2 | ~$0 (local GPU) | Full fine-tune yolov5n/s on MOCS+ACID(+SODA), 100–300 epochs @640px. Overnight on the 4070. |
| 3 | labeling time | Fine-tune on frames from the customer's actual site. Biggest real-world win. |

## Repo layout

```
data/        class taxonomy + YOLOv5 dataset configs
scripts/     dataset download / COCO→YOLO / VOC→YOLO prep (hardlinking)
export/      Axis TFLite INT8 export pipeline + float-vs-INT8 eval
steering/    off-camera PTZ steering controller (MQTT in → VAPIX out)
acap/        native steering ACAP (later port of steering/)
docs/        plan, hardware findings
```

## Workflow (all training scripts run inside WSL2 Ubuntu)

```bash
# one-time environment (micromamba py3.11, patched yolov5, torch CUDA, TF 2.15)
bash scripts/setup_wsl_env.sh

# Stage 1: dataset → train → export → eval
~/machinery/env/bin/python scripts/prep_stage1_hf.py --out ~/machinery/datasets/stage1
bash scripts/train_stage1.sh       # EPOCHS=60 BATCH=16 by default
bash scripts/export_stage1.sh      # INT8 TFLite → models/stage1/
bash scripts/eval_stage1.sh        # float vs INT8 mAP (healthy PTQ loss: 1-3 pts)
```

Deploy to the camera (from Windows):

```bash
powershell -ExecutionPolicy Bypass -File scripts/deploy_model.ps1 -Tflite models/stage1/machinery_stage1_int8.tflite -Labels models/stage1/labels.txt -Description "Stage 1 machinery fine-tune"
```

Check live detections: `http://<camera>/local/detectx/status` (digest auth).

Steering (needs a PTZ camera + MQTT broker; test dome is fixed):

```bash
python steering/ptz_tracker.py --config steering/config.yaml --dry-run
```

## License note

YOLOv5 is AGPL-3.0 (Ultralytics). Fine for the PoC; a commercial ACAP needs an
Ultralytics license or a permissive-license architecture swap later.
