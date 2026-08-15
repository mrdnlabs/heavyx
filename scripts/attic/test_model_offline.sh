#!/usr/bin/env bash
# Sanity-test the deployed INT8 tflite: run it over val images that contain
# machinery (excavators=2, dump truck=13, wheel loader=16) and save annotated
# outputs to the Windows repo for eyeballing.
set -euo pipefail

WORK=$HOME/machinery
PY=$WORK/env/bin/python
YOLO=$WORK/yolov5
DS=$WORK/datasets/stage1
TFLITE=$WORK/runs/stage1/weights/best-int8.tflite
OUT=/mnt/c/20260812_ConstructionYoloModel/models/stage1/test_detections
PICK=$WORK/datasets/stage1/machinery_val
CONF=${CONF:-0.35}

mkdir -p "$PICK"
rm -f "$PICK"/*

# collect val+test images whose labels contain machinery classes
for lbl in "$DS"/labels/val/*.txt "$DS"/labels/test/*.txt; do
    if grep -qE '^(2|13|16) ' "$lbl"; then
        stem=$(basename "$lbl" .txt)
        split=$(basename "$(dirname "$lbl")")
        img="$DS/images/$split/$stem.jpg"
        [ -f "$img" ] && cp "$img" "$PICK/"
    fi
done
echo "picked $(ls "$PICK" | wc -l) machinery images"

cd "$YOLO"
export TORCH_FORCE_NO_WEIGHTS_ONLY_LOAD=1
"$PY" detect.py --weights "$TFLITE" --data "$DS/stage1.yaml" \
    --source "$PICK" --imgsz 640 --conf-thres "$CONF" \
    --project "$WORK/runs" --name detect_int8 --exist-ok

mkdir -p "$OUT"
rm -f "$OUT"/*
cp "$WORK/runs/detect_int8"/*.jpg "$OUT/" 2>/dev/null || true
echo "== annotated results in $OUT =="
