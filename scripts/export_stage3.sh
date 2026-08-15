#!/usr/bin/env bash
# Export Stage 3 yolov5s to INT8 TFLite for ARTPEC-9. Uses the ORIGINAL env
# (torch 2.1.2 + TF 2.13.1 per the Axis recipe) — export is CPU-only, so the
# old torch that crashes CUDA training is harmless here.
set -euo pipefail

WORK=$HOME/machinery
PY=$WORK/env/bin/python
YOLO=$WORK/yolov5
DATA=$WORK/datasets/stage3/stage3.yaml
WEIGHTS=${1:-$WORK/runs/stage3/weights/best.pt}
OUT=/mnt/c/20260812_ConstructionYoloModel/models/stage3

export TORCH_FORCE_NO_WEIGHTS_ONLY_LOAD=1
export YOLOv5_AUTOINSTALL=false

cd "$YOLO"
"$PY" export.py --weights "$WEIGHTS" --include tflite --int8 --data "$DATA" --imgsz 640

mkdir -p "$OUT"
cp -v "${WEIGHTS%.pt}-int8.tflite" "$OUT/machinery_stage3_int8.tflite"
cp -v "$WORK/datasets/stage3/labels.txt" "$OUT/labels.txt"
echo "== artifacts in $OUT =="
