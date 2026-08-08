from __future__ import annotations

import argparse
import difflib
import hashlib
import json
import re
import shutil
from collections import Counter
from pathlib import Path


QUALITY = ("Cracked", "Flawed", "Fine", "Perfect", "Royal", "Ultimate")
RARITIES = ("Poor", "Common", "Uncommon", "Rare", "Epic", "Legendary", "Unique", "Artifact")
CLASSES = ("Fighter", "Barbarian", "Rogue", "Ranger", "Wizard", "Cleric",
           "Bard", "Warlock", "Druid", "Sorcerer", "Paladin")
META_VALUES = {
    "Slot Type": ("Invalid", "Primary Weapon", "Secondary Weapon", "Head(Open helmet)",
                  "Head(Full helmet)", "Head(Hat)", "Chest", "Hands", "Legs", "Feet",
                  "Back", "Utility", "Necklace", "Ring"),
    "Hand Type": ("One-Handed", "Two-Handed"),
    "Weapon Type": ("Sword", "Axe", "Mace", "Polearm", "Crossbow", "Bow", "Dagger",
                    "Magic Stuff", "Musical Instrument", "Shield"),
    "Armor Type": ("Plate", "Leather", "Cloth"),
    "Utility Type": ("Consumable", "Installable", "Throwable", "Drink", "Light Source",
                     "Mining", "Musical Instrument"),
    "Loot State": ("Handled", "Looted", "Supplied"),
    "Rarity": RARITIES,
}


def ascii_text(text: str) -> str:
    return text.translate(str.maketrans({
        "’": "'", "‘": "'", "“": '"', "”": '"',
        "−": "-", "–": "-", "—": "-", "…": "...",
    }))


def normalized(text: str) -> str:
    text = ascii_text(text)
    return re.sub(r"[^a-z0-9]+", "", text.lower())


def ratio(a: str, b: str) -> float:
    return difflib.SequenceMatcher(None, normalized(a), normalized(b)).ratio()


def closest(text: str, values: list[str] | tuple[str, ...]) -> tuple[str, float]:
    scored = [(ratio(text, value), value) for value in values]
    score, value = max(scored, default=(0.0, text))
    return value, score


def partition_words(text: str, observed: list[str]) -> list[str]:
    """Put authoritative words back onto the observed number of visual rows."""
    words = text.split()
    if not observed or not words:
        return observed
    count = min(len(observed), len(words))
    # Dynamic programming over line breaks, minimizing observed character-width error.
    inf = 10**9
    dp = [[inf] * (len(words) + 1) for _ in range(count + 1)]
    prev = [[-1] * (len(words) + 1) for _ in range(count + 1)]
    dp[0][0] = 0
    for line in range(1, count + 1):
        for end in range(line, len(words) + 1):
            for start in range(line - 1, end):
                if dp[line - 1][start] == inf:
                    continue
                candidate = " ".join(words[start:end])
                cost = dp[line - 1][start] + abs(len(candidate) - len(observed[line - 1])) ** 2
                if cost < dp[line][end]:
                    dp[line][end], prev[line][end] = cost, start
    result, end = [], len(words)
    for line in range(count, 0, -1):
        start = prev[line][end]
        result.append(" ".join(words[start:end]))
        end = start
    result.reverse()
    return result


def title_candidates(catalog: dict, observed: str) -> tuple[list[str], dict[str, dict]]:
    by_name: dict[str, dict] = {}
    for item in catalog["items"]:
        name = item.get("name")
        if name:
            by_name.setdefault(name.replace("’", "'"), item)
    names = list(by_name)
    suffix = re.search(r"\(([^)]+)\)\s*$", observed)
    if suffix:
        names += [f"{name} ({quality})" for name in by_name for quality in QUALITY]
    return names, by_name


def canonical_item(candidate: str, by_name: dict[str, dict]) -> dict | None:
    base = re.sub(r"\s+\((?:" + "|".join(QUALITY) + r")\)$", "", candidate)
    return by_name.get(base)


def split_for(title: str, seed: str) -> str:
    bucket = int(hashlib.sha256((seed + "\0" + normalized(title)).encode()).hexdigest()[:8], 16) % 100
    return "test" if bucket < 15 else "valid" if bucket < 30 else "train"


def stat_vocabulary(catalog: dict, verified_manifest: Path) -> list[str]:
    values = {item.get("name", "") for item in catalog.get("attributes", [])}
    values.update({"Armor Rating", "Weapon Damage", "Magic Weapon Damage", "Magical Damage",
                   "Move Speed", "Move Speed Bonus", "Magic Resistance", "Headshot Damage Reduction",
                   "Projectile Damage Reduction", "Armor Penetration", "Action Speed", "Strength",
                   "Dexterity", "Agility", "Vigor", "Will", "Knowledge", "Resourcefulness", "Luck",
                   "All Attributes", "Max Health", "Magical Power", "Magical Healing"})
    if verified_manifest.exists():
        for line in verified_manifest.read_text(encoding="utf-8").splitlines():
            text = line.split("\t", 1)[-1]
            match = re.match(r"^[+-]\d+(?:\.\d+)?%?\s+(.+?)(?:\s+\[[^]]+\])?$", text)
            if match:
                values.add(match.group(1))
            match = re.match(r"^(.+?)\s+-?\d+(?:\.\d+)?%?$", text)
            if match and len(match.group(1).split()) <= 5:
                values.add(match.group(1))
    return sorted(value for value in values if value)


def canonical_stat(text: str, vocabulary: list[str], confidence: float) -> tuple[str, bool]:
    if confidence < .94:
        return text, False
    prefix = re.match(r"^([+-]\d+(?:\.\d+)?%?)\s+(.+?)(\s+\[[^]]+\])?$", text)
    if prefix:
        name, score = closest(prefix.group(2), vocabulary)
        if score >= .84:
            return prefix.group(1) + " " + name + (prefix.group(3) or ""), True
    suffix = re.match(r"^(.+?)\s+(-?\d+(?:\.\d+)?%?)$", text)
    if suffix:
        name, score = closest(suffix.group(1), vocabulary)
        if score >= .84:
            return name + " " + suffix.group(2), True
    return text, False


def main() -> None:
    here = Path(__file__).resolve().parent
    parser = argparse.ArgumentParser(description="Freeze and bootstrap-label the persistent OCR inbox")
    parser.add_argument("--inbox", type=Path,
                        default=Path("/mnt/c/Users/Ethan/AppData/Local/GrimVault/ocr-samples/inbox"))
    parser.add_argument("--catalog", type=Path, default=here/"cache/ddb-catalog.json")
    parser.add_argument("--details", type=Path, default=here/"cache/ddb-inbox-details.json")
    parser.add_argument("--output", type=Path, default=here/"inbox-dataset-20260716")
    parser.add_argument("--seed", default="grimvault-ocr-v1")
    parser.add_argument("--manual", type=Path,
                        help="optional file<TAB>label overrides; use __SKIP__ to exclude a crop")
    args = parser.parse_args()
    args.manual = args.manual or args.output / "manual.tsv"
    catalog = json.loads(args.catalog.read_text(encoding="utf-8"))
    detail_items = json.loads(args.details.read_text(encoding="utf-8")).get("items", {}) \
        if args.details.exists() else {}
    details_by_name_rarity = {(item.get("name", "").replace("’", "'"), item.get("rarity", "")): item
                              for item in detail_items.values()}
    stats = stat_vocabulary(catalog, here/"real-train.tsv")

    samples = []
    seen_tooltips: set[str] = set()
    for metadata_path in sorted(args.inbox.glob("*/metadata.json")):
        metadata = json.loads(metadata_path.read_text(encoding="utf-8"))
        if metadata["tooltip_pixel_hash"] in seen_tooltips:
            continue
        seen_tooltips.add(metadata["tooltip_pixel_hash"])
        samples.append((metadata_path.parent, metadata))

    args.output.mkdir(parents=True, exist_ok=True)
    crop_root = args.output / "crops"
    names, by_name = title_candidates(catalog, "")
    records = []
    summary = Counter()
    manual: dict[str, str] = {}
    if args.manual.exists():
        for raw in args.manual.read_text(encoding="utf-8").splitlines():
            if not raw.strip() or raw.lstrip().startswith("#"):
                continue
            name, label = raw.split("\t", 1)
            manual[name] = label

    for source_dir, metadata in samples:
        lines = metadata.get("lines", [])
        title_line = next((line for line in lines if line.get("title")), None)
        observed_title = title_line.get("prediction", "") if title_line else ""
        candidates, _ = title_candidates(catalog, observed_title)
        title, title_score = closest(observed_title, candidates)
        observed_key, title_key = normalized(observed_title), normalized(title)
        title_accepted = (title_score >= .70 and len(observed_key) >= 2 and
                          .55 <= len(observed_key) / max(1, len(title_key)) <= 1.45 and
                          (title_line or {}).get("confidence", 0.0) >= .78)
        item = canonical_item(title, by_name) if title_accepted else None
        observed_rarity = next((line.get("prediction", "")[8:].lower() for line in lines
                                if line.get("prediction", "").startswith("Rarity: ")), "")
        detail = details_by_name_rarity.get((re.sub(
            r"\s+\((?:" + "|".join(QUALITY) + r")\)$", "", title), observed_rarity), {})
        group_split = split_for(title, args.seed)
        destination = crop_root / metadata["id"]
        destination.mkdir(parents=True, exist_ok=True)

        labels = []
        for line in lines:
            prediction = ascii_text(line.get("prediction", ""))
            label, source, verified = prediction, "prediction", False
            if line.get("title") and title_accepted:
                label, source, verified = title, "ddb:title", True
            else:
                for prefix, values in META_VALUES.items():
                    if ratio(prediction.split(":", 1)[0], prefix) >= .72 and ":" in prediction:
                        value, value_score = closest(prediction.split(":", 1)[1], values)
                        if value_score >= .62:
                            label, source, verified = f"{prefix}: {value}", "grammar:metadata", True
                        break
                if prediction == "Required Class:":
                    label, source, verified = prediction, "grammar:metadata", True
                elif prediction and all(part.strip(" .,\"") in CLASSES
                                        for part in prediction.split(",")):
                    label, source, verified = prediction.replace(".", ""), "grammar:classes", True
                if not verified:
                    stat, stat_ok = canonical_stat(prediction, stats, line.get("confidence", 0.0))
                    if stat_ok:
                        label, source, verified = stat, "grammar:stat", True

            target = destination / line["file"]
            shutil.copy2(source_dir / line["file"], target)
            labels.append({
                "file": f"{metadata['id']}/{line['file']}", "prediction": prediction,
                "label": label, "verified": verified, "source": source,
                "confidence": line.get("confidence", 0.0), "title": bool(line.get("title")),
                "source_band": line.get("source_band"), "pixel_hash": line.get("pixel_hash"),
            })

        # Flavor text is authoritative and appears as a wrapped contiguous block
        # near the end, immediately before an optional ownership footer.
        flavor = ascii_text((item or {}).get("flavor", "")).strip()
        if flavor:
            footer_at = next((i for i, row in enumerate(labels)
                              if re.match(r'^"(?:Found|Crafted) by ', row["prediction"])), len(labels))
            best = None
            for start in range(1, footer_at):
                observed = " ".join(row["prediction"] for row in labels[start:footer_at])
                score = ratio(observed, flavor)
                if best is None or score > best[0]:
                    best = (score, start)
            if best and best[0] >= .72:
                wrapped = partition_words(flavor, [row["prediction"] for row in labels[best[1]:footer_at]])
                for row, value in zip(labels[best[1]:footer_at], wrapped):
                    row.update(label=value, verified=True, source="ddb:flavor")

        # Effects are rarity-specific. Find the best still-pending contiguous
        # block before metadata, and use the API text only when it strongly
        # agrees with the captured rows. Preserve whichever parenthesis spacing
        # variant is visually closest to the current prediction.
        effect = ascii_text(detail.get("effect", "")).strip()
        if effect:
            variants = [effect, re.sub(r"(?<=\d)(?=\()", " ", effect)]
            first_meta = next((i for i, row in enumerate(labels)
                               if row["source"] == "grammar:metadata"), len(labels))
            best_effect = None
            for start in range(1, first_meta):
                for end in range(start + 1, min(first_meta, start + 16) + 1):
                    observed_rows = labels[start:end]
                    if any(row["verified"] for row in observed_rows):
                        continue
                    observed = " ".join(row["prediction"] for row in observed_rows)
                    for variant in variants:
                        size_ratio = len(normalized(observed)) / max(1, len(normalized(variant)))
                        if not .6 <= size_ratio <= 1.45:
                            continue
                        score = ratio(observed, variant)
                        if best_effect is None or score > best_effect[0]:
                            best_effect = (score, start, end, variant)
            if best_effect and best_effect[0] >= .78:
                _, start, end, variant = best_effect
                wrapped = partition_words(variant, [row["prediction"] for row in labels[start:end]])
                for row, value in zip(labels[start:end], wrapped):
                    row.update(label=value, verified=True, source="ddb:effect")

        for row in labels:
            override = manual.get(row["file"])
            row["include"] = override != "__SKIP__"
            if override is not None and override != "__SKIP__":
                row.update(label=override, verified=True, source="manual")

        for row in labels:
            state = "excluded" if not row["include"] else "verified" if row["verified"] else "pending"
            summary[(group_split, state)] += 1
            records.append({"sample_id": metadata["id"], "split": group_split,
                            "canonical_title": title, "title_score": title_score, **row})

    with (args.output / "labels.jsonl").open("w", encoding="utf-8") as stream:
        for record in records:
            stream.write(json.dumps(record, ensure_ascii=False) + "\n")
    for split in ("train", "valid", "test"):
        with (args.output / f"{split}.tsv").open("w", encoding="utf-8") as stream:
            for record in records:
                if record["split"] == split and record["verified"] and record["include"]:
                    stream.write(f"{record['file']}\t{record['label']}\n")
    report = {
        "source_samples": len(list(args.inbox.glob("*/metadata.json"))),
        "unique_tooltips": len(samples), "records": len(records),
        "summary": {f"{split}:{state}": count for (split, state), count in sorted(summary.items())},
    }
    (args.output / "summary.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
