#!/usr/bin/env bash
# Stage 2: fine-tune yolov5s (Axis ARTPEC-9 COCO checkpoint) on the unified
# machinery dataset (~5.5k images, 5 classes). Overnight-friendly on the 4070.
# yolov5s runs at ~35 ms on the ARTPEC-9 DLPU — still ~28 fps, worth the
# accuracy bump over yolov5n.
set -euo pipefail

WORK=$HOME/machinery
# prefer the modern-torch training env (see setup_train_env.sh); the shared
# env's torch 2.1.2 crashes on the 2026 driver under WSL2
PY=$WORK/env-train/bin/python
[ -x "$PY" ] || PY=$WORK/env/bin/python
YOLO=$WORK/yolov5
DATA=${DATA:-$WORK/datasets/stage2/stage2.yaml}
WEIGHTS=${WEIGHTS:-/mnt/c/20260812_ConstructionYoloModel/models/stage0/yolov5s_artpec9_coco_640.pt}

# batch 12 / workers 2: an early run at batch 16 died at epoch 9 with a CUDA
# illegal-memory-access — WSL2 VRAM contention with the active desktop (8 GB
# card shared with browser/monitors). Lower pressure = stable overnight run.
EPOCHS=${EPOCHS:-100}
BATCH=${BATCH:-12}
NAME=${NAME:-stage2}

export TORCH_FORCE_NO_WEIGHTS_ONLY_LOAD=1
export YOLOv5_AUTOINSTALL=false   # don't let yolov5 pip-install TF into the train env

# fail fast if the env has been corrupted (see setup_train_env.sh notes)
"$PY" -c "import torch, typing_extensions, numpy, pandas" || {
    echo "PREFLIGHT FAILED: train env is broken — rerun setup_train_env.sh" >&2
    exit 1
}
echo "preflight ok: $("$PY" -c 'import torch,numpy; print(torch.__version__, numpy.__version__)') AUTOINSTALL=$YOLOv5_AUTOINSTALL"

cd "$YOLO"
"$PY" train.py \
    --name "$NAME" \
    --exist-ok \
    --data "$DATA" \
    --weights "$WEIGHTS" \
    --epochs "$EPOCHS" \
    --batch-size "$BATCH" \
    --imgsz 640 \
    --workers 2 \
    --project "$WORK/runs"

echo "== done: best weights at $WORK/runs/$NAME/weights/best.pt =="
