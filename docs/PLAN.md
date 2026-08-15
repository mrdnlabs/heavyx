# Staged plan and status

Goal: an Axis camera that detects heavy construction machinery and (on a PTZ
unit) auto-steers to follow it. Slow targets → 100–300 ms detect-to-steer
latency is fine.

## Hardware

| Camera | Role | SoC | Verdict |
|--------|------|-----|---------|
| AXIS P3408-VE (LAN test unit) | fixed dome, detection testbed | ARTPEC-9 DLPU | runs HeavyX 1.0.x (detection + overlay; steering auto-disabled) |
| AXIS Q6355-LE (to procure) | PTZ for steering demo + site | ARTPEC-9 DLPU | recommended |
| AXIS P5655-E (customer site today) | ARTPEC-7, no DLPU | not viable for YOLO (~211 ms SSD only) | replace or add edge box |

## Stages

- **Stage 0 — deploy path validation: DONE 2026-08-12.**
  Axis pretrained `yolov5n_artpec9_coco_640.tflite` deployed to P3408-VE via
  DetectX 4.1.1. Runs on DLPU at ~45 ms/frame (~22 fps). Detections visible on
  `/local/detectx/status`. Install required temporarily enabling
  "allow unsigned apps" (re-disabled right after).
- **Stage 1 — training path validation (in progress).**
  Fine-tune yolov5n from the Axis ARTPEC-9 COCO checkpoint on
  keremberke/construction-safety-object-detection (398 imgs, 17 classes incl.
  excavators / dump truck / wheel loader). Export INT8 TFLite, compare
  float-vs-INT8 mAP (healthy PTQ loss: 1–3 points), deploy to test camera.
- **Stage 2 — real model.** MOCS (~41.7k imgs) + ACID (~10k) (+ SODA ~19.8k),
  unified machinery taxonomy, 100–300 epochs yolov5n/s @640 on the local
  RTX 4070 (overnight). QAT only if PTQ disappoints.
- **Stage 3 — site adaptation.** Label a few hundred frames from the customer's
  site footage, fine-tune. Biggest real-world accuracy win.
- **Steering** (needs PTZ hardware): `steering/ptz_tracker.py` consumes DetectX
  MQTT detections and drives VAPIX continuous moves. Start with an off-camera
  controller, port to a native ACAP once tuned.

## Toolchain facts (verified)

- Recipe: `third_party/axis-model-zoo/docs/yolov5.md` — yolov5 @ commit
  `95ebf68f`, ARTPEC-9 patch from `acap-ml-models.s3.amazonaws.com`, train,
  `export.py --include tflite --int8` (per-channel OK on ARTPEC-9; ARTPEC-8
  would need `--per-tensor`).
- Axis pretrained ARTPEC-9 checkpoints exist (yolov5n/s/m COCO 640) — both
  `.pt` (fine-tune start) and `.tflite` (instant deploy). In `models/stage0/`.
- Training env: WSL2 Ubuntu, micromamba python 3.11, torch 2.6.0 CUDA
  (needs `TORCH_FORCE_NO_WEIGHTS_ONLY_LOAD=1` for yolov5's torch.load),
  tensorflow-cpu for the TFLite export. Env at `~/machinery/env`,
  repo at `~/machinery/yolov5` (patched), runs at `~/machinery/runs`.
- DetectX model upload: JSON POST to `/local/detectx/model` with
  `{description, tflite_b64, labels_content}` — automated in
  `scripts/deploy_model.ps1`. App restarts itself and reports
  `customModel:true` + per-frame `averageTime` in `/local/detectx/status`.
- DetectX detection payload: pixel coords in its video frame (856×640 on the
  P3408-VE, "coordinateVersion 2"); MQTT topics `detectx/detection/<serial>`,
  events `detectx/event/<serial>/<label>/<state>`.
- AXIS OS 12 blocks unsigned ACAPs: toggle via
  `/axis-cgi/applications/config.cgi?action=set&name=AllowUnsigned&value=true|false`.

## Dataset access findings (2026-08-13)

- **MOCS** (41.7k): gated behind a wjx.cn survey form (or CodaLab account);
  license CC BY-**NC** 4.0 — non-commercial. Kaggle mirror exists
  (xiaopan9802/mocs-dataset) but needs Kaggle creds.
- **ACID** (10k): Google-form request, ~1 week turnaround, research-only.
- **HF LouisChen15/ConstructionSite** (10k re-annotated MOCS): gated; user's
  cached HF token was rejected (expired?).
- **Used for Stage 2 instead (ungated)**: keremberke/excavator-detector
  (~2.7k imgs) + Francesco/excavators-czvg9 (RF100, 2.7k) + Stage 1 set
  → ~5.5k images, unified 5-class taxonomy
  (excavator, dump truck, wheel loader, truck, person).
  `scripts/prep_stage2.py` builds it; dropped labels become background
  negatives.
- NC/research-only licenses on MOCS/ACID matter for the commercial ACAP —
  Stage 3 (own site footage) sidesteps this entirely.

## Open items

1. Procure ARTPEC-9 PTZ (Q6355-LE) for the steering half.
3. MOCS is CC BY-NC and ACID research-only — PoC use fine; keep out of the
   commercial model lineage (customer-footage retrain is the clean path).
4. Ultralytics AGPL: PoC fine; commercial ACAP needs license or arch swap.
