from __future__ import annotations

import argparse
import csv
import json
from collections import Counter
from pathlib import Path

import cv2


def default_inbox() -> Path:
    return Path("/mnt/c/Users/Ethan/AppData/Local/GrimVault/ocr-samples/inbox")


def difference_hash(path: Path) -> int:
    image = cv2.imread(str(path), cv2.IMREAD_GRAYSCALE)
    if image is None:
        raise ValueError(f"unreadable image: {path}")
    small = cv2.resize(image, (17, 16), interpolation=cv2.INTER_AREA)
    bits = small[:, 1:] > small[:, :-1]
    value = 0
    for bit in bits.flat:
        value = (value << 1) | int(bit)
    return value


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Build a non-destructive OCR inbox review manifest")
    parser.add_argument("--inbox", type=Path, default=default_inbox())
    parser.add_argument("--output", type=Path, default=Path("inbox-review.tsv"))
    parser.add_argument("--near-distance", type=int, default=4,
                        help="maximum 256-bit dHash distance for a near-duplicate")
    args = parser.parse_args()

    rows: list[dict] = []
    for metadata_path in sorted(args.inbox.glob("*/metadata.json")):
        metadata = json.loads(metadata_path.read_text(encoding="utf-8"))
        sample_dir = metadata_path.parent
        for line in metadata.get("lines", []):
            image_path = sample_dir / line["file"]
            if not image_path.exists():
                continue
            rows.append({
                "sample_id": metadata["id"],
                "line_index": line["index"],
                "source_band": line["source_band"],
                "title": line["title"],
                "prediction": line.get("prediction", ""),
                "confidence": float(line.get("confidence", 0.0)),
                "pixel_hash": line["pixel_hash"],
                "image": str(image_path.resolve()),
                "dhash": difference_hash(image_path),
            })

    exact_counts = Counter(row["pixel_hash"] for row in rows)
    representatives: dict[str, dict] = {}
    for row in rows:
        current = representatives.get(row["pixel_hash"])
        if current is None or row["confidence"] < current["confidence"]:
            # Retain the least-confident occurrence for review; it carries the
            # most information when identical pixels produced varied output.
            representatives[row["pixel_hash"]] = row

    unique = sorted(representatives.values(), key=lambda row: (
        row["confidence"], not row["title"], row["sample_id"], row["line_index"]))
    cluster_hashes: list[int] = []
    for row in unique:
        cluster = None
        for index, candidate in enumerate(cluster_hashes):
            if (row["dhash"] ^ candidate).bit_count() <= args.near_distance:
                cluster = index
                break
        if cluster is None:
            cluster = len(cluster_hashes)
            cluster_hashes.append(row["dhash"])
        row["near_cluster"] = cluster
        row["exact_occurrences"] = exact_counts[row["pixel_hash"]]

    args.output.parent.mkdir(parents=True, exist_ok=True)
    fields = ["sample_id", "line_index", "source_band", "title", "prediction",
              "confidence", "exact_occurrences", "near_cluster", "pixel_hash", "image"]
    with args.output.open("w", encoding="utf-8", newline="") as stream:
        writer = csv.DictWriter(stream, fields, delimiter="\t", extrasaction="ignore")
        writer.writeheader()
        writer.writerows(unique)
    print(f"samples={len({row['sample_id'] for row in rows})} "
          f"lines={len(rows)} exact_unique={len(unique)} "
          f"near_clusters={len(cluster_hashes)} output={args.output}")


if __name__ == "__main__":
    main()
