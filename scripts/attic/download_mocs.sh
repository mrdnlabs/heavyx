#!/usr/bin/env bash
# Download the MOCS dataset (links issued via the wjx.cn request form,
# 2026-08-13). Google Drive hosted — gdown handles the confirm-token dance.
# License: CC BY-NC 4.0 — PoC/research use only, keep out of commercial models.
set -euo pipefail

WORK=$HOME/machinery
PY=$WORK/env/bin/python
DEST=$WORK/datasets/mocs_raw
mkdir -p "$DEST"

"$PY" -m pip install --quiet gdown

cd "$DEST"
declare -A FILES=(
    [annotation_train.zip]=1Oh0rIq2YmCZHFSTWXOpedYSE06c8Sq9j
    [annotation_val.zip]=18Q7ugoRJZ8ntjM07pwqkDhWGxPvQAgBt
    [image_info_test.zip]=1yYJOdXF9SbuvU_BcVsuLb-mrgmUL5Pmj
    [val_images.zip]=1Zeqr7C5p-hWNw5ta2fvD1bgLnXd-17fW
    [test_images.zip]=1Uj9-oZFIAk9Jy_JGMfNZlLbERplEXj-i
    [train_images.zip]=1kzlZLdH31nm6QsTOusuL2DJnQDQbTKDt
)
# small files first so failures surface early; train_images (the giant) last
for name in annotation_train.zip annotation_val.zip image_info_test.zip \
            val_images.zip test_images.zip train_images.zip; do
    id=${FILES[$name]}
    if [ -f "$name" ]; then
        echo "== $name already present, skipping =="
        continue
    fi
    echo "== downloading $name ($id) =="
    "$PY" -m gdown "https://drive.google.com/uc?id=$id" -O "$name"
done
echo "== MOCS download complete =="
ls -la "$DEST"
