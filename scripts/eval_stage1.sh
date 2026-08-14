#!/usr/bin/env bash
# Compare float (.pt) vs INT8 (.tflite) accuracy on the Stage 1 val split.
# Healthy PTQ loss is 1-3 mAP points; a cliff means calibration went wrong.
set -euo pipefail

WORK=$HOME/machinery
PY=$WORK/env/bin/python
YOLO=$WORK/yolov5
DATA=$WORK/datasets/stage1/stage1.yaml
BEST=${1:-$WORK/runs/stage1/weights/best.pt}

export TORCH_FORCE_NO_WEIGHTS_ONLY_LOAD=1
cd "$YOLO"

echo "=== FLOAT (best.pt) ==="
"$PY" val.py --weights "$BEST" --data "$DATA" --imgsz 640 --task val \
    --project "$WORK/runs" --name val_float --exist-ok

echo "=== INT8 (best-int8.tflite, CPU — slow) ==="
"$PY" val.py --weights "${BEST%.pt}-int8.tflite" --data "$DATA" --imgsz 640 --task val \
    --batch-size 1 --project "$WORK/runs" --name val_int8 --exist-ok
