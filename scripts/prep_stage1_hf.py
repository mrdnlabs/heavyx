#!/usr/bin/env python3
"""Download keremberke/construction-safety-object-detection (398 images,
classes incl. excavators / dump truck / wheel loader / truck) straight from
the HF repo's data zips and convert Roboflow-COCO annotations to YOLOv5 layout.

Run inside the WSL training env:
    python prep_stage1_hf.py --out ~/machinery/datasets/stage1
"""
import argparse
import json
import os
import urllib.request
import zipfile
from pathlib import Path

BASE = ("https://huggingface.co/datasets/"
        "keremberke/construction-safety-object-detection/resolve/main/data")
SPLITS = {"train": "train.zip", "valid": "valid.zip", "test": "test.zip"}
YOLO_SPLIT = {"train": "train", "valid": "val", "test": "test"}


def coco_to_yolo(coco: dict, lbl_dir: Path, names_out: list) -> int:
    # Roboflow COCO exports carry a dummy supercategory (id 0); real classes
    # point their supercategory at it. Filter it out, keep id order.
    cats = [c for c in coco["categories"] if c.get("supercategory") != "none"]
    if not cats:
        cats = coco["categories"]
    cats = sorted(cats, key=lambda c: c["id"])
    id2idx = {c["id"]: i for i, c in enumerate(cats)}
    if not names_out:
        names_out.extend(c["name"] for c in cats)

    images = {im["id"]: im for im in coco["images"]}
    per_image = {}
    for ann in coco["annotations"]:
        if ann["category_id"] not in id2idx:
            continue
        im = images[ann["image_id"]]
        x, y, bw, bh = ann["bbox"]
        cx, cy = (x + bw / 2) / im["width"], (y + bh / 2) / im["height"]
        line = (f"{id2idx[ann['category_id']]} "
                f"{cx:.6f} {cy:.6f} {bw / im['width']:.6f} {bh / im['height']:.6f}")
        per_image.setdefault(ann["image_id"], []).append(line)

    n = 0
    for im_id, im in images.items():
        stem = Path(im["file_name"]).stem
        lines = per_image.get(im_id, [])
        (lbl_dir / f"{stem}.txt").write_text("\n".join(lines) + "\n")
        n += len(lines)
    return n


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True)
    args = ap.parse_args()
    out = Path(os.path.expanduser(args.out))
    out.mkdir(parents=True, exist_ok=True)
    names: list = []

    for split, zname in SPLITS.items():
        ysplit = YOLO_SPLIT[split]
        zpath = out / zname
        if not zpath.exists():
            print(f"downloading {zname} ...")
            urllib.request.urlretrieve(f"{BASE}/{zname}", zpath)
        img_dir = out / "images" / ysplit
        lbl_dir = out / "labels" / ysplit
        img_dir.mkdir(parents=True, exist_ok=True)
        lbl_dir.mkdir(parents=True, exist_ok=True)

        with zipfile.ZipFile(zpath) as zf:
            members = zf.namelist()
            coco_name = next(m for m in members if m.endswith("_annotations.coco.json"))
            coco = json.loads(zf.read(coco_name))
            n_img = 0
            for m in members:
                if m.lower().endswith((".jpg", ".jpeg", ".png")):
                    (img_dir / Path(m).name).write_bytes(zf.read(m))
                    n_img += 1
        n_box = coco_to_yolo(coco, lbl_dir, names)
        print(f"{ysplit}: {n_img} images, {n_box} boxes")

    yaml_lines = [
        f"path: {out}",
        "train: images/train",
        "val: images/val",
        "test: images/test",
        "names:",
    ] + [f"  {i}: {n}" for i, n in enumerate(names)]
    (out / "stage1.yaml").write_text("\n".join(yaml_lines) + "\n")
    (out / "labels.txt").write_text("\n".join(names) + "\n")
    print(f"{len(names)} classes: {names}")
    print(f"wrote {out / 'stage1.yaml'} and {out / 'labels.txt'}")


if __name__ == "__main__":
    main()
