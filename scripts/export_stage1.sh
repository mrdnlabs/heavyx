#!/usr/bin/env bash
# Export the Stage 1 fine-tuned model to INT8 TFLite for ARTPEC-9 and copy the
# artifacts to the Windows repo. Run inside WSL2 Ubuntu after train_stage1.sh.
# ARTPEC-9 uses per-channel quantization (the default; ARTPEC-8 would need --per-tensor).
set -euo pipefail

WORK=$HOME/machinery
PY=$WORK/env/bin/python
YOLO=$WORK/yolov5
DATA=$WORK/datasets/stage1/stage1.yaml
WEIGHTS=${1:-$WORK/runs/stage1/weights/best.pt}
OUT=/mnt/c/20260812_ConstructionYoloModel/models/stage1

export TORCH_FORCE_NO_WEIGHTS_ONLY_LOAD=1

cd "$YOLO"
"$PY" export.py --weights "$WEIGHTS" --include tflite --int8 --data "$DATA" --imgsz 640

mkdir -p "$OUT"
cp -v "${WEIGHTS%.pt}-int8.tflite" "$OUT/machinery_stage1_int8.tflite"
cp -v "$WORK/datasets/stage1/labels.txt" "$OUT/labels.txt"
echo "== artifacts in $OUT =="
