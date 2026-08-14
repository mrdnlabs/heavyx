#!/usr/bin/env python3
"""Build the Stage 3 PoC dataset: MOCS (41.7k imgs, CC BY-NC — PoC only!)
merged with the Stage 2 corpus (keremberke + stage1 + COCO backfill).

Taxonomy grows to 12 classes; the first five keep their Stage 2 ids so the
stage2 labels can be hardlinked unchanged:

  0 excavator      4 person          8 tower crane
  1 dump truck     5 bulldozer       9 pump truck
  2 wheel loader   6 roller         10 concrete mixer
  3 truck          7 mobile crane   11 pile driver

Run inside WSL:
    python prep_stage3.py --mocs ~/machinery/datasets/mocs_raw \
        --stage2 ~/machinery/datasets/stage2 --out ~/machinery/datasets/stage3
"""
import argparse
import json
import os
import zipfile
from pathlib import Path

CLASSES = ["excavator", "dump truck", "wheel loader", "truck", "person",
           "bulldozer", "roller", "mobile crane", "tower crane", "pump truck",
           "concrete mixer", "pile driver"]

# MOCS category names (several naming variants seen in the wild) -> ours
MOCS_MAP = {
    "worker": "person",
    "excavator": "excavator",
    "truck": "truck",
    "loader": "wheel loader",
    "bulldozer": "bulldozer",
    "dozer": "bulldozer",
    "roller": "roller",
    "crane": "mobile crane",
    "vehicle crane": "mobile crane",
    "mobile crane": "mobile crane",
    "static crane": "tower crane",
    "tower crane": "tower crane",
    "pump truck": "pump truck",
    "concrete pump": "pump truck",
    "concrete mixer": "concrete mixer",
    "concrete transport mixer": "concrete mixer",
    "pile driving": "pile driver",
    "pile driver": "pile driver",
    # dropped: hanging head/hook, other vehicle
}


def convert_mocs_split(ann_path: Path, img_index: dict, split: str, out: Path,
                       holdout_every: int = 0) -> None:
    """Convert one MOCS annotation file. If holdout_every=N, every Nth image
    goes to the val split instead (gives us a real-world val set)."""
    for s in ("train", "val"):
        (out / "images" / s).mkdir(parents=True, exist_ok=True)
        (out / "labels" / s).mkdir(parents=True, exist_ok=True)

    coco = json.loads(ann_path.read_text())
    id2our = {}
    unmapped = set()
    for c in coco["categories"]:
        name = c["name"].strip().lower()
        target = MOCS_MAP.get(name)
        if target is None:
            for k, v in MOCS_MAP.items():
                if k in name:
                    target = v
                    break
        if target is None:
            unmapped.add(c["name"])
        else:
            id2our[c["id"]] = CLASSES.index(target)
    if unmapped:
        print(f"  (dropping unmapped MOCS classes: {sorted(unmapped)})")

    images = {im["id"]: im for im in coco["images"]}
    per_image: dict = {im_id: [] for im_id in images}
    n_box = 0
    for a in coco["annotations"]:
        cls = id2our.get(a["category_id"])
        if cls is None or a.get("iscrowd"):
            continue
        im = images[a["image_id"]]
        x, y, bw, bh = a["bbox"]
        w, h = im["width"], im["height"]
        if bw <= 1 or bh <= 1:
            continue
        per_image[a["image_id"]].append(
            f"{cls} {(x+bw/2)/w:.6f} {(y+bh/2)/h:.6f} {bw/w:.6f} {bh/h:.6f}")
        n_box += 1

    n_img = n_miss = 0
    for i, (im_id, im) in enumerate(sorted(images.items())):
        fname = Path(im["file_name"]).name
        src = img_index.get(fname)
        if src is None:
            n_miss += 1
            continue
        dest_split = "val" if holdout_every and i % holdout_every == 0 else split
        stem = f"mocs_{Path(fname).stem}"
        dest = out / "images" / dest_split / f"{stem}{Path(fname).suffix.lower()}"
        if not dest.exists():
            os.link(src, dest)
        (out / "labels" / dest_split / f"{stem}.txt").write_text(
            "\n".join(per_image[im_id]) + "\n")
        n_img += 1
    print(f"MOCS {ann_path.name}: {n_img} images converted, {n_box} boxes"
          + (f", {n_miss} without image files (train zip pending)" if n_miss else ""))


def link_stage2(stage2: Path, out: Path) -> None:
    n = 0
    for split in ("train", "val", "test"):
        for kind in ("images", "labels"):
            src_dir = stage2 / kind / split
            if not src_dir.is_dir():
                continue
            dst_dir = out / kind / split
            dst_dir.mkdir(parents=True, exist_ok=True)
            for f in src_dir.iterdir():
                dest = dst_dir / f.name
                if not dest.exists():
                    os.link(f, dest)
                    n += 1
    print(f"stage2 corpus linked: {n} files")


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--mocs", required=True)
    ap.add_argument("--stage2", required=True)
    ap.add_argument("--out", required=True)
    args = ap.parse_args()
    mocs = Path(os.path.expanduser(args.mocs))
    out = Path(os.path.expanduser(args.out))
    out.mkdir(parents=True, exist_ok=True)

    # extract annotation + image zips that haven't been extracted yet
    # (test_images.zip is skipped: no public annotations)
    for z in list(mocs.glob("annotation_*.zip")) + \
             [p for p in (mocs / "train_images.zip", mocs / "val_images.zip")
              if p.exists()]:
        marker = mocs / f".extracted_{z.stem}"
        if not marker.exists():
            print(f"extracting {z.name} ...")
            try:
                with zipfile.ZipFile(z) as zf:
                    zf.extractall(mocs / z.stem)
            except zipfile.BadZipFile:
                # MOCS "zips" are actually RAR v4 — use conda-forge 7z
                import subprocess
                seven_zip = Path.home() / "machinery/env/bin/7z"
                subprocess.run([str(seven_zip), "x", "-y",
                                f"-o{mocs / z.stem}", str(z)],
                               check=True, capture_output=True)
            marker.touch()

    # index every extracted image once: filename -> path
    img_index = {}
    for sub in ("train_images", "val_images"):
        d = mocs / sub
        if d.is_dir():
            for p in d.rglob("*"):
                if p.suffix.lower() in (".jpg", ".jpeg", ".png"):
                    img_index[p.name] = p
    print(f"indexed {len(img_index)} MOCS image files")

    def find_json(pattern: str) -> Path:
        hits = [p for p in sorted(mocs.rglob(pattern))
                if p.suffix == ".json" and "info" not in p.name.lower()]
        if not hits:
            raise SystemExit(f"no annotation json matching {pattern} under {mocs}")
        return hits[0]

    link_stage2(Path(os.path.expanduser(args.stage2)), out)
    convert_mocs_split(find_json("*train*.json"), img_index, "train", out)
    # MOCS val: 80% into train, every 5th image held out into our val split
    convert_mocs_split(find_json("*val*.json"), img_index, "train", out,
                       holdout_every=5)

    yaml_lines = [
        f"path: {out}", "train: images/train", "val: images/val",
        "test: images/test", "names:",
    ] + [f"  {i}: {n}" for i, n in enumerate(CLASSES)]
    (out / "stage3.yaml").write_text("\n".join(yaml_lines) + "\n")
    (out / "labels.txt").write_text("\n".join(CLASSES) + "\n")
    for split in ("train", "val"):
        n = len(list((out / "images" / split).glob("*")))
        print(f"TOTAL {split}: {n} images")


if __name__ == "__main__":
    main()
