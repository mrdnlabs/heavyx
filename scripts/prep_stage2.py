#!/usr/bin/env python3
"""Build the Stage 2 machinery dataset (~5.5k images) from three open sources:

  1. keremberke/excavator-detector      (HF, Roboflow-COCO zips)
  2. Francesco/excavators-czvg9         (HF, RF100, parquet w/ COCO bboxes)
  3. the Stage 1 set already on disk    (YOLO layout from prep_stage1_hf.py)

Everything is remapped to a unified 5-class machinery taxonomy. Images whose
labels are all dropped are kept as background negatives (empty label files).

Run inside the WSL training env:
    python prep_stage2.py --out ~/machinery/datasets/stage2 \
        --stage1 ~/machinery/datasets/stage1
"""
import argparse
import io
import json
import os
import urllib.request
import zipfile
from pathlib import Path

CLASSES = ["excavator", "dump truck", "wheel loader", "truck", "person"]
# source label (lowercased) -> unified name; anything absent is dropped
MAP = {
    "excavator": "excavator",
    "excavators": "excavator",
    "dump truck": "dump truck",
    "wheel loader": "wheel loader",
    "truck": "truck",
    "person": "person",
}

KEREM = ("https://huggingface.co/datasets/keremberke/excavator-detector"
         "/resolve/main/data")
FRANC = ("https://huggingface.co/datasets/Francesco/excavators-czvg9"
         "/resolve/main/data")
FRANC_FILES = {
    "train": "train-00000-of-00001-6d7a240744f05f9a.parquet",
    "val": "validation-00000-of-00001-5431c142cc7106a8.parquet",
    "test": "test-00000-of-00001-48fb4219fd925aa3.parquet",
}


def fetch(url: str, dest: Path) -> None:
    if dest.exists():
        return
    print(f"downloading {dest.name} ...")
    urllib.request.urlretrieve(url, dest)


def yolo_line(cls: int, x: float, y: float, bw: float, bh: float,
              w: int, h: int) -> str:
    return (f"{cls} {(x + bw / 2) / w:.6f} {(y + bh / 2) / h:.6f} "
            f"{bw / w:.6f} {bh / h:.6f}")


def add_roboflow_coco_zip(zpath: Path, split: str, prefix: str, out: Path) -> tuple:
    img_dir = out / "images" / split
    lbl_dir = out / "labels" / split
    img_dir.mkdir(parents=True, exist_ok=True)
    lbl_dir.mkdir(parents=True, exist_ok=True)
    n_img = n_box = 0
    with zipfile.ZipFile(zpath) as zf:
        coco_name = next(m for m in zf.namelist()
                         if m.endswith("_annotations.coco.json"))
        coco = json.loads(zf.read(coco_name))
        cats = {c["id"]: MAP.get(c["name"].strip().lower())
                for c in coco["categories"]}
        images = {im["id"]: im for im in coco["images"]}
        per_image = {im_id: [] for im_id in images}
        for ann in coco["annotations"]:
            uni = cats.get(ann["category_id"])
            if uni is None:
                continue
            im = images[ann["image_id"]]
            x, y, bw, bh = ann["bbox"]
            per_image[ann["image_id"]].append(
                yolo_line(CLASSES.index(uni), x, y, bw, bh,
                          im["width"], im["height"]))
        for m in zf.namelist():
            if not m.lower().endswith((".jpg", ".jpeg", ".png")):
                continue
            im_entry = next((im for im in images.values()
                             if im["file_name"] == Path(m).name), None)
            stem = f"{prefix}_{Path(m).stem}"
            (img_dir / f"{stem}{Path(m).suffix.lower()}").write_bytes(zf.read(m))
            lines = per_image.get(im_entry["id"], []) if im_entry else []
            (lbl_dir / f"{stem}.txt").write_text("\n".join(lines) + "\n")
            n_img += 1
            n_box += len(lines)
    return n_img, n_box


def add_parquet(ppath: Path, split: str, prefix: str, out: Path,
                names: list) -> tuple:
    import pandas as pd

    img_dir = out / "images" / split
    lbl_dir = out / "labels" / split
    img_dir.mkdir(parents=True, exist_ok=True)
    lbl_dir.mkdir(parents=True, exist_ok=True)
    df = pd.read_parquet(ppath)
    n_img = n_box = 0
    for i, row in df.iterrows():
        img_bytes = row["image"]["bytes"]
        from PIL import Image
        with Image.open(io.BytesIO(img_bytes)) as im:
            w, h = im.size
            stem = f"{prefix}_{split}_{i:06d}"
            if im.mode != "RGB":
                im = im.convert("RGB")
            im.save(img_dir / f"{stem}.jpg", quality=92)
        objs = row["objects"]
        lines = []
        for bbox, cat in zip(objs["bbox"], objs["category"]):
            uni = MAP.get(names[int(cat)].strip().lower())
            if uni is None:
                continue
            x, y, bw, bh = [float(v) for v in bbox]
            lines.append(yolo_line(CLASSES.index(uni), x, y, bw, bh, w, h))
        (lbl_dir / f"{stem}.txt").write_text("\n".join(lines) + "\n")
        n_img += 1
        n_box += len(lines)
    return n_img, n_box


def add_stage1(stage1: Path, out: Path) -> tuple:
    s1_names = (stage1 / "labels.txt").read_text().splitlines()
    n_img = n_box = 0
    for split in ("train", "val", "test"):
        img_src = stage1 / "images" / split
        lbl_src = stage1 / "labels" / split
        if not img_src.is_dir():
            continue
        img_dir = out / "images" / split
        lbl_dir = out / "labels" / split
        img_dir.mkdir(parents=True, exist_ok=True)
        lbl_dir.mkdir(parents=True, exist_ok=True)
        for img in img_src.iterdir():
            stem = f"s1_{img.stem}"
            os.link(img, img_dir / f"{stem}{img.suffix}")
            lines = []
            lbl = lbl_src / f"{img.stem}.txt"
            if lbl.exists():
                for ln in lbl.read_text().split("\n"):
                    if not ln.strip():
                        continue
                    parts = ln.split()
                    uni = MAP.get(s1_names[int(parts[0])].strip().lower())
                    if uni is not None:
                        lines.append(" ".join([str(CLASSES.index(uni))] + parts[1:]))
            (lbl_dir / f"{stem}.txt").write_text("\n".join(lines) + "\n")
            n_img += 1
            n_box += len(lines)
    return n_img, n_box


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True)
    ap.add_argument("--stage1", required=True)
    args = ap.parse_args()
    out = Path(os.path.expanduser(args.out))
    dl = out / "_downloads"
    dl.mkdir(parents=True, exist_ok=True)

    # 1. keremberke zips (Roboflow-COCO; valid split -> our val)
    for zname, split in (("train.zip", "train"), ("valid.zip", "val"),
                         ("test.zip", "test")):
        fetch(f"{KEREM}/{zname}", dl / f"kerem_{zname}")
        n = add_roboflow_coco_zip(dl / f"kerem_{zname}", split, "kx", out)
        print(f"keremberke {split}: {n[0]} images, {n[1]} boxes")

    # NOTE: Francesco/excavators-czvg9 (RF100) turned out to be the SAME
    # dataset as keremberke/excavator-detector (identical image/box counts,
    # splits renamed) — including it double-counted train and made val==test.
    # add_parquet() is kept for future parquet-format sources.

    # 3. stage 1 set (hardlink, remap)
    n = add_stage1(Path(os.path.expanduser(args.stage1)), out)
    print(f"stage1 merged: {n[0]} images, {n[1]} boxes")

    yaml_lines = [
        f"path: {out}",
        "train: images/train",
        "val: images/val",
        "test: images/test",
        "names:",
    ] + [f"  {i}: {n}" for i, n in enumerate(CLASSES)]
    (out / "stage2.yaml").write_text("\n".join(yaml_lines) + "\n")
    (out / "labels.txt").write_text("\n".join(CLASSES) + "\n")
    for split in ("train", "val", "test"):
        n = len(list((out / "images" / split).glob("*")))
        print(f"TOTAL {split}: {n} images")
    print("classes:", CLASSES)


if __name__ == "__main__":
    main()
