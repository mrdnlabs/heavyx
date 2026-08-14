#!/usr/bin/env bash
# Crash-resilient training wrapper. The WSL2 GPU stack intermittently throws
# CUDA illegal-instruction/illegal-memory-access under sustained load (seen on
# torch 2.1.2 AND 2.6.0), so: launch training once, then on every crash resume
# via yolov5's --resume (restores epoch/optimizer/LR) until it completes or
# MAX_RETRIES is hit.
#
# Usage: NAME=stage2m BATCH=6 WEIGHTS=<start.pt> bash train_resume_loop.sh
set -uo pipefail

WORK=$HOME/machinery
PY=$WORK/env-train/bin/python
YOLO=$WORK/yolov5
NAME=${NAME:-stage2m}
MAX_RETRIES=${MAX_RETRIES:-15}
LAST=$WORK/runs/$NAME/weights/last.pt

export TORCH_FORCE_NO_WEIGHTS_ONLY_LOAD=1
export YOLOv5_AUTOINSTALL=false

attempt=0
while [ "$attempt" -le "$MAX_RETRIES" ]; do
    if [ ! -f "$LAST" ]; then
        # no checkpoint yet (first run, or crashed inside epoch 0) — start fresh
        echo "[$(date -Is)] attempt $attempt: fresh start"
        bash /mnt/c/20260812_ConstructionYoloModel/scripts/train_stage2.sh
    else
        echo "[$(date -Is)] attempt $attempt: --resume $LAST"
        cd "$YOLO"
        "$PY" train.py --resume "$LAST"
    fi
    rc=$?
    if [ "$rc" -eq 0 ]; then
        echo "[$(date -Is)] training completed after $attempt resume(s)"
        echo "== done: best weights at $WORK/runs/$NAME/weights/best.pt =="
        exit 0
    fi
    attempt=$((attempt + 1))
    echo "[$(date -Is)] crashed (rc=$rc) — resuming in 20s (attempt $attempt/$MAX_RETRIES)"
    sleep 20
done
echo "[$(date -Is)] gave up after $MAX_RETRIES resumes" >&2
exit 1
