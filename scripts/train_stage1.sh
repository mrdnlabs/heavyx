#!/usr/bin/env bash
# Stage 1: fine-tune yolov5n (Axis ARTPEC-9 pretrained COCO checkpoint) on the
# construction-safety dataset. Run inside WSL2 Ubuntu.
set -euo pipefail

WORK=$HOME/machinery
PY=$WORK/env/bin/python
YOLO=$WORK/yolov5
DATA=$WORK/datasets/stage1/stage1.yaml
WEIGHTS=/mnt/c/20260812_ConstructionYoloModel/models/stage0/yolov5n_artpec9_coco_640.pt

EPOCHS=${EPOCHS:-60}
BATCH=${BATCH:-16}

export TORCH_FORCE_NO_WEIGHTS_ONLY_LOAD=1   # yolov5 torch.load vs torch>=2.6

cd "$YOLO"
"$PY" train.py \
    --name stage1 \
    --exist-ok \
    --data "$DATA" \
    --weights "$WEIGHTS" \
    --epochs "$EPOCHS" \
    --batch-size "$BATCH" \
    --imgsz 640 \
    --workers 2 \
    --project "$WORK/runs"

echo "== done: best weights at $WORK/runs/stage1/weights/best.pt =="
