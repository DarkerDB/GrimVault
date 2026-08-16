from __future__ import annotations

import argparse
import json
import re
from pathlib import Path

from PIL import Image


def split_for(path: Path) -> str:
    match = re.search(r"tooltip_(\d+)", path.stem)
    ordinal = int(match.group(1)) if match else 0
    return "val" if ((ordinal - 1) // 8) % 5 == 4 else "train"


def prepare(source: Path, destination: Path) -> None:
    labels = source / "labeled_screenshots" / "labels" / "train"
    images = source / "screenshots"
    records = {"train": [], "val": []}
    annotations = {"train": [], "val": []}
    image_ids = {"train": 1, "val": 1}
    annotation_ids = {"train": 1, "val": 1}

    for split in records:
        (destination / f"{split}2017").mkdir(parents=True, exist_ok=True)
        (destination / "annotations").mkdir(parents=True, exist_ok=True)

    for source_image in sorted(images.glob("*.png")):
        source_label = labels / f"{source_image.stem}.txt"
        if not source_label.exists():
            continue

        split = split_for(source_image)
        image_id = image_ids[split]
        image_ids[split] += 1
        output_name = f"{source_image.stem}.jpg"
        output_image = destination / f"{split}2017" / output_name

        with Image.open(source_image) as image:
            image = image.convert("RGB")
            if image.width > 1720:
                image = image.resize((1720, round(image.height * 1720 / image.width)), Image.Resampling.LANCZOS)
            width, height = image.size
            image.save(output_image, quality=90, optimize=True, progressive=True)

        records[split].append({"id": image_id, "file_name": output_name, "width": width, "height": height})
        for line in source_label.read_text(encoding="utf-8").splitlines():
            class_id, center_x, center_y, box_width, box_height = map(float, line.split())
            if int(class_id) != 0:
                raise ValueError(f"unexpected class in {source_label}: {class_id}")
            x = (center_x - box_width / 2) * width
            y = (center_y - box_height / 2) * height
            w = box_width * width
            h = box_height * height
            annotations[split].append({
                "id": annotation_ids[split],
                "image_id": image_id,
                "category_id": 1,
                "bbox": [x, y, w, h],
                "area": w * h,
                "iscrowd": 0,
                "segmentation": [],
            })
            annotation_ids[split] += 1

    for split in records:
        payload = {
            "info": {"description": "GrimVault tooltip detector"},
            "licenses": [],
            "images": records[split],
            "annotations": annotations[split],
            "categories": [{"id": 1, "name": "tooltip", "supercategory": "tooltip"}],
        }
        output = destination / "annotations" / f"instances_{split}2017.json"
        output.write_text(json.dumps(payload, separators=(",", ":")), encoding="utf-8")
        print(f"{split}: {len(records[split])} images, {len(annotations[split])} boxes")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("source", type=Path)
    parser.add_argument("--destination", type=Path, default=Path(__file__).parent / "data")
    args = parser.parse_args()
    prepare(args.source.resolve(), args.destination.resolve())


if __name__ == "__main__":
    main()
