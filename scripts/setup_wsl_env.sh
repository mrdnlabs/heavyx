#!/usr/bin/env bash
# Set up the YOLOv5 (Axis ARTPEC-9 patched) training environment inside WSL2 Ubuntu.
# Everything heavy lives on the WSL ext4 filesystem (~/machinery) — /mnt/c IO is slow.
# No sudo required: uses micromamba for python, pip for packages.
set -euo pipefail

WORK=$HOME/machinery
YOLO=$WORK/yolov5
MAMBA=$WORK/micromamba
ENVDIR=$WORK/env
YOLO_COMMIT=95ebf68f92196975e53ebc7e971d0130432ad107
PATCH_URL=https://acap-ml-models.s3.amazonaws.com/yolov5/yolov5_artpec9.patch

mkdir -p "$WORK"

# --fresh: rebuild the python env from scratch
if [ "${1:-}" = "--fresh" ] && [ -d "$ENVDIR" ]; then
    echo "== removing existing env for fresh rebuild =="
    rm -rf "$ENVDIR"
fi

# --- python 3.11 via micromamba (self-contained, no sudo) ---
if [ ! -x "$MAMBA" ]; then
    echo "== downloading micromamba =="
    curl -Ls https://micro.mamba.pm/api/micromamba/linux-64/latest \
        | tar -xj -C "$WORK" --strip-components=1 bin/micromamba
    mv "$WORK/micromamba" "$MAMBA" 2>/dev/null || true
fi
if [ ! -d "$ENVDIR" ]; then
    echo "== creating python 3.11 env =="
    "$MAMBA" create -y -p "$ENVDIR" -c conda-forge python=3.11 pip
fi
PY="$ENVDIR/bin/python"

# --- yolov5 at Axis-pinned commit + ARTPEC-9 patch ---
if [ ! -d "$YOLO" ]; then
    echo "== cloning yolov5 @ $YOLO_COMMIT =="
    git clone https://github.com/ultralytics/yolov5 "$YOLO"
    git -C "$YOLO" checkout "$YOLO_COMMIT"
    echo "== applying ARTPEC-9 patch =="
    curl -Ls "$PATCH_URL" -o "$WORK/yolov5_artpec9.patch"
    git -C "$YOLO" apply "$WORK/yolov5_artpec9.patch"
fi

# --- dependencies ---
# Single pip invocation = single resolver pass = consistent versions.
# torch 2.6 PyPI wheels are CUDA-enabled; yolov5's torch.load needs
# TORCH_FORCE_NO_WEIGHTS_ONLY_LOAD=1 at runtime with torch>=2.6.
# TF comes from the Axis-patched requirements.txt (pins tensorflow<=2.13.1,
# a Keras-2 release — yolov5's models/tf.py tflite export breaks on Keras 3).
# torch must be 2.1.x: TF 2.13 pins typing-extensions<4.6, torch>=2.2 needs newer.
echo "== installing python packages (torch CUDA, TF for tflite export) =="
"$PY" -m pip install --quiet --upgrade pip
"$PY" -m pip install --quiet \
    torch==2.1.2 torchvision==0.16.2 \
    -r "$YOLO/requirements.txt" \
    "setuptools<81" \
    datasets pillow tqdm

# --- sanity checks ---
echo "== sanity =="
export TORCH_FORCE_NO_WEIGHTS_ONLY_LOAD=1
"$PY" - <<'EOF'
import torch
print("torch", torch.__version__, "| cuda available:", torch.cuda.is_available(),
      "|", torch.cuda.get_device_name(0) if torch.cuda.is_available() else "NO GPU")
import tensorflow as tf
print("tensorflow", tf.__version__)
EOF
git -C "$YOLO" log --oneline -1
git -C "$YOLO" status --short | head -5
echo "== setup complete: env=$ENVDIR yolo=$YOLO =="
