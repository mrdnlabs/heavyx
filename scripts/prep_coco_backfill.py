#!/usr/bin/env python3
"""Backfill weak classes (person, truck) from COCO 2017 (CC BY 4.0).

Downloads the COCO annotations, samples N images containing person and M
containing truck, fetches just those images over HTTP, and writes YOLO labels
into the stage2 dataset (train split) using the unified taxonomy. All other
COCO objects in those images are ignored (unlabeled background) EXCEPT other
vehicles, whose boxes are labeled to avoid teaching the model that visible
trucks/persons elsewhere are background.

Run inside WSL:  python prep_coco_backfill.py --out ~/machinery/datasets/stage2
"""
import argparse
import io
import json
import os
import random
import urllib.request
import zipfile
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

ANN_URL = "http://images.cocodataset.org/annotations/annotations_trainval2017.zip"
IMG_BASE = "http://images.cocodataset.org/train2017"

# COCO name -> our class
OUR = {"person": 4, "truck": 3}
CLASS_NAMES = ["excavator", "dump truck", "wheel loader", "truck", "person"]


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True)
    ap.add_argument("--persons", type=int, default=1400)
    ap.add_argument("--trucks", type=int, default=800)
    ap.add_argument("--seed", type=int, default=17)
    args = ap.parse_args()
    out = Path(os.path.expanduser(args.out))
    dl = out / "_downloads"
    dl.mkdir(parents=True, exist_ok=True)
    ann_zip = dl / "annotations_trainval2017.zip"
    if not ann_zip.exists():
        print("downloading COCO annotations (241 MB)...")
        urllib.request.urlretrieve(ANN_URL, ann_zip)

    print("parsing instances_train2017.json ...")
    with zipfile.ZipFile(ann_zip) as zf:
        coco = json.loads(zf.read("annotations/instances_train2017.json"))

    cat_by_id = {c["id"]: c["name"] for c in coco["categories"]}
    images = {im["id"]: im for im in coco["images"]}
    anns_by_img: dict = {}
    for a in coco["annotations"]:
        if a.get("iscrowd"):
            continue
        anns_by_img.setdefault(a["image_id"], []).append(a)

    person_imgs, truck_imgs = [], []
    for img_id, anns in anns_by_img.items():
        names = {cat_by_id[a["category_id"]] for a in anns}
        if "truck" in names:
            truck_imgs.append(img_id)
        elif "person" in names:
            person_imgs.append(img_id)

    rng = random.Random(args.seed)
    picks = (rng.sample(truck_imgs, min(args.trucks, len(truck_imgs))) +
             rng.sample(person_imgs, min(args.persons, len(person_imgs))))
    print(f"selected {len(picks)} COCO images "
          f"({len(truck_imgs)} truck candidates, {len(person_imgs)} person)")

    img_dir = out / "images" / "train"
    lbl_dir = out / "labels" / "train"

    def grab(img_id: int) -> bool:
        im = images[img_id]
        stem = f"coco_{im['file_name'].removesuffix('.jpg')}"
        dest = img_dir / f"{stem}.jpg"
        try:
            if not dest.exists():
                urllib.request.urlretrieve(f"{IMG_BASE}/{im['file_name']}", dest)
        except Exception as e:
            print(f"skip {im['file_name']}: {e}")
            return False
        lines = []
        for a in anns_by_img[img_id]:
            cls = OUR.get(cat_by_id[a["category_id"]])
            if cls is None:
                continue
            x, y, bw, bh = a["bbox"]
            w, h = im["width"], im["height"]
            lines.append(f"{cls} {(x+bw/2)/w:.6f} {(y+bh/2)/h:.6f} "
                         f"{bw/w:.6f} {bh/h:.6f}")
        (lbl_dir / f"{stem}.txt").write_text("\n".join(lines) + "\n")
        return True

    with ThreadPoolExecutor(max_workers=12) as ex:
        ok = sum(ex.map(grab, picks))
    print(f"added {ok} COCO images to {img_dir}")
    n = len(list(img_dir.glob('*')))
    print(f"train split now {n} images")


if __name__ == "__main__":
    main()
