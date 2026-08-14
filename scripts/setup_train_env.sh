#!/usr/bin/env bash
# Separate TRAINING env with modern torch. Rationale: the shared env pins
# TF 2.13 (Axis export recipe) whose typing-extensions<4.6 forces torch down
# to 2.1.2 — and torch 2.1.2's cu121 kernels crash (illegal instruction /
# illegal memory access) on the 2026 NVIDIA 595 driver under WSL2.
# Training needs no TF, so it gets torch 2.6 here; export keeps the old env.
set -euo pipefail

WORK=$HOME/machinery
MAMBA=$WORK/micromamba
ENVDIR=$WORK/env-train
YOLO=$WORK/yolov5

if [ ! -d "$ENVDIR" ]; then
    "$MAMBA" create -y -p "$ENVDIR" -c conda-forge python=3.11 pip
fi
PY="$ENVDIR/bin/python"

# yolov5 requirements minus the tensorflow line (export-only dependency)
grep -viE '^\s*tensorflow' "$YOLO/requirements.txt" > /tmp/req_train.txt

"$PY" -m pip install --quiet --upgrade pip
"$PY" -m pip install --quiet \
    torch==2.6.0 torchvision==0.21.0 \
    -r /tmp/req_train.txt \
    "setuptools<81" "numpy<2"   # old yolov5 uses np.trapz, removed in numpy 2

# belt-and-suspenders: some yolov5 code paths shell out to bare `pip install`
# which can drag typing-extensions below torch 2.6's floor — re-pin last
"$PY" -m pip install --quiet -U "typing-extensions>=4.10"

export TORCH_FORCE_NO_WEIGHTS_ONLY_LOAD=1
"$PY" - <<'EOF'
import torch
print("torch", torch.__version__, "| cuda:", torch.cuda.is_available(),
      "|", torch.cuda.get_device_name(0) if torch.cuda.is_available() else "NO GPU")
EOF
echo "== train env ready: $ENVDIR =="
