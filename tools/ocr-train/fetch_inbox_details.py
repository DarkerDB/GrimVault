from __future__ import annotations

import argparse
import json
import re
from pathlib import Path

from fetch_ddb_catalog import onsite_key, request_json


QUALITY = ("Cracked", "Flawed", "Fine", "Perfect", "Royal", "Ultimate")


def base_title(title: str) -> str:
    return re.sub(r"\s+\((?:" + "|".join(QUALITY) + r")\)$", "", title)


def main() -> None:
    here = Path(__file__).resolve().parent
    parser = argparse.ArgumentParser(description="Fetch DDB details for frozen inbox items")
    parser.add_argument("--dataset", type=Path, default=here/"inbox-dataset-20260716")
    parser.add_argument("--catalog", type=Path, default=here/"cache/ddb-catalog.json")
    parser.add_argument("--inbox", type=Path,
                        default=Path("/mnt/c/Users/Ethan/AppData/Local/GrimVault/ocr-samples/inbox"))
    parser.add_argument("--web-root", type=Path,
                        default=Path.home()/".katforge/realms/darkerdb.com")
    parser.add_argument("--base", default="https://api.dev.darkerdb.com")
    parser.add_argument("--output", type=Path, default=here/"cache/ddb-inbox-details.json")
    args = parser.parse_args()

    labels = [json.loads(line) for line in (args.dataset/"labels.jsonl").read_text().splitlines()]
    title_by_sample = {row["sample_id"]: base_title(row["canonical_title"])
                       for row in labels if row["title"]}
    rarity_by_sample = {}
    for metadata_path in args.inbox.glob("*/metadata.json"):
        metadata = json.loads(metadata_path.read_text())
        rarity = next((line.get("prediction", "")[8:].lower()
                       for line in metadata.get("lines", [])
                       if line.get("prediction", "").startswith("Rarity: ")), "")
        rarity_by_sample[metadata["id"]] = rarity

    catalog = json.loads(args.catalog.read_text())
    by_name_rarity = {(item.get("name", "").replace("’", "'"), item.get("rarity", "")): item
                      for item in catalog["items"] if item.get("name")}
    ids = set()
    for sample_id, title in title_by_sample.items():
        item = by_name_rarity.get((title, rarity_by_sample.get(sample_id, "")))
        if item:
            ids.add(item["id"])

    key = onsite_key(args.web_root)
    details = {}
    for index, item_id in enumerate(sorted(ids), 1):
        envelope = request_json(args.base, "/v2/items/" + item_id, key,
                                {"locale": "en", "condense": "true"})
        details[item_id] = envelope.get("body", {})
        if index % 20 == 0:
            print(f"fetched={index}/{len(ids)}", flush=True)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps({"items": details}, ensure_ascii=False, indent=2))
    print(f"details={len(details)} output={args.output}")


if __name__ == "__main__":
    main()
