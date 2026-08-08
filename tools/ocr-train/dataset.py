from __future__ import annotations

import csv
import json
import random
from pathlib import Path

import numpy as np
import cv2
import torch
from PIL import Image, ImageDraw, ImageEnhance, ImageFilter, ImageFont
from torch.utils.data import Dataset

HEIGHT = 48
WIDTH = 960
ASCII = "".join(chr(i) for i in range(33, 127))


def production_normalize(image: Image.Image) -> np.ndarray:
    """Bit-for-bit equivalent of PaddleRecognizer::preprocess luminance LUT."""
    rgb = np.asarray(image.convert("RGB"))
    gray = cv2.cvtColor(rgb, cv2.COLOR_RGB2GRAY)
    histogram = np.bincount(gray.ravel(), minlength=256)
    cumulative = np.cumsum(histogram)
    pixels = gray.size
    background = int(np.searchsorted(cumulative, pixels // 2, side="left"))
    foreground = int(np.searchsorted(cumulative, pixels * 49 // 50, side="left"))
    span = max(24, foreground - background)
    values = np.arange(256, dtype=np.float64)
    normalized = np.sqrt(np.clip((values - background) / span, 0.0, 1.0))
    # std::lround is half-away-from-zero; all values here are non-negative.
    lut = np.floor(255.0 * normalized + 0.5).astype(np.uint8)
    return lut[gray]


def production_title_image(image: Image.Image) -> Image.Image:
    """Mirror C++ title-rule removal for preserved pre-recognition bands."""
    rgb = np.asarray(image.convert("RGB"))
    gray = np.asarray(image.convert("L"))
    if gray.shape[1] < gray.shape[0] * 8:
        return image
    histogram = np.bincount(gray.ravel(), minlength=256)
    background = int(np.searchsorted(np.cumsum(histogram), gray.size // 2))
    threshold = min(72, max(24, background + 18))
    mask = gray > threshold
    # Preserved debug bands may already be tightly column-trimmed. A long
    # title can then occupy half a row; only a near-full-width stroke is a
    # separator in this representation.
    if max(np.count_nonzero(row) for row in mask) <= mask.shape[1] * 4 // 5:
        return image
    cut = None
    for y in range(4, mask.shape[0]):
        if np.count_nonzero(mask[y]) <= mask.shape[1] // 2:
            continue
        cut = y
        while cut > 0 and np.count_nonzero(mask[cut - 1]) > mask.shape[1] // 8:
            cut -= 1
        break
    if cut is None or cut < 6:
        return image
    rgb = rgb[:cut]
    mask = mask[:cut]
    columns = np.flatnonzero(mask.any(axis=0))
    if columns.size:
        x0 = max(0, int(columns[0]) - 4)
        x1 = min(rgb.shape[1], int(columns[-1]) + 5)
        rgb = rgb[:, x0:x1]
    return Image.fromarray(rgb)

COLORS = [
    (245, 245, 240), (207, 199, 181), (78, 72, 67), (9, 174, 246),
    (240, 154, 34), (72, 220, 20), (185, 105, 68), (244, 206, 20),
]
RARITIES = ["Poor", "Common", "Uncommon", "Rare", "Epic", "Legendary", "Unique"]
CLASSES = ["Fighter", "Barbarian", "Rogue", "Ranger", "Wizard", "Cleric",
           "Bard", "Warlock", "Druid", "Sorcerer", "Paladin"]
METADATA = {
    "Slot Type": ["Invalid", "Primary Weapon", "Secondary Weapon", "Head", "Chest",
                  "Hands", "Legs", "Foot", "Back", "Utility", "Necklace", "Ring"],
    "Hand Type": ["One-Handed", "Two-Handed"],
    "Weapon Type": ["Sword", "Axe", "Mace", "Polearm", "Crossbow", "Bow", "Dagger"],
    "Armor Type": ["Plate", "Leather", "Cloth"],
    "Utility Type": ["Consumable", "Installable", "Throwable"],
    "Loot State": ["Handled", "Looted"],
    "Rarity": RARITIES,
}
FALLBACK_PROSE = [
    "Constructed by melding a piercing metal tip to a strong wooden pole, giving it a long deadly reach.",
    "These are small metal fragments left behind during the equipment dismantling process.",
    "A dirty chunk of rock bearing traces of Tidestone with teal wave-like patterns.",
    "Heals 22 (0.5) recoverable health. Takes 4 seconds to apply the bandage.",
    "The shards of this gem shines a deep blue light.",
    "Bandage used to fix a wound.",
    "Required for a current quest",
    "Found by EF0000",
]


def vocabulary(ddb: Path) -> tuple[list[str], list[str], list[str]]:
    stats: set[str] = {
        "Weapon Damage", "Move Speed", "Additional Weapon Damage", "Physical Power",
        "Magical Damage Bonus", "Action Speed", "Armor Penetration", "Luck", "Vigor",
    }
    for name in ("attributes.csv", "effects.csv"):
        path = ddb / "data" / name
        if path.exists():
            with path.open(encoding="utf-8-sig", newline="") as stream:
                for row in csv.DictReader(stream):
                    if row.get("display"): stats.add(row["display"].strip())

    items: set[str] = set()
    prose: set[str] = set()
    root = (ddb / "data/Game/Exports/DungeonCrawler/Content/DungeonCrawler/"
            "Data/Generated/DT_Item/Item")
    for path in root.glob("*.json"):
        try:
            document = json.loads(path.read_text(encoding="utf-8"))
            item = document[0]["Properties"]["Item"]
            title = item["Name"]["LocalizedString"].strip()
            if title and all(c in ASCII or c == " " for c in title):
                items.add(title)
            flavor = item.get("FlavorText", {}).get("LocalizedString", "").strip()
            if flavor and all(c in ASCII or c in " \n" for c in flavor):
                prose.add(" ".join(flavor.split()))
        except (OSError, UnicodeDecodeError, json.JSONDecodeError, KeyError,
                IndexError, TypeError):
            continue
    # Current DDB builds no longer need to keep Unreal export JSON in the web
    # realm. Use the explicitly fetched, patch-pinned API cache when those
    # development exports are absent. This data only shapes synthetic training;
    # it is never shipped or consulted by runtime OCR.
    if not items:
        cache = Path(__file__).resolve().parent / "cache/ddb-catalog.json"
        if cache.exists():
            catalog = json.loads(cache.read_text(encoding="utf-8"))
            for item in catalog.get("items", []):
                title = str(item.get("name", "")).strip()
                flavor = str(item.get("flavor", "")).strip()
                if title and all(c in ASCII or c == " " for c in title):
                    items.add(title)
                if flavor and all(c in ASCII or c in " \n" for c in flavor):
                    prose.add(" ".join(flavor.split()))
            for attribute in catalog.get("attributes", []):
                display = str(attribute.get("name", "")).strip()
                if display:
                    stats.add(display)
        if not items:
            raise RuntimeError(f"no localized item records found under {root} or API cache")
    return sorted(items), sorted(stats), sorted(prose or FALLBACK_PROSE)


def wrap_lines(text: str, rng: random.Random, maximum: int = 62) -> list[str]:
    words, lines, current = text.split(), [], ""
    for word in words:
        proposed = f"{current} {word}".strip()
        if current and len(proposed) > maximum:
            lines.append(current); current = word
        else:
            current = proposed
    if current: lines.append(current)
    return lines


class TooltipLines(Dataset):
    def __init__(self, count: int, seed: int, ddb: Path, font_dir: Path) -> None:
        self.count, self.seed = count, seed
        self.items, self.stats, self.prose = vocabulary(ddb)
        self.fonts = {
            "light": font_dir / "SaintKDG_Light.ttf",
            "medium": font_dir / "SaintKDG_Medium.ttf",
            "flavor": font_dir / "Pelagiad.ttf",
        }
        missing = [str(p) for p in self.fonts.values() if not p.exists()]
        if missing: raise FileNotFoundError("missing fonts: " + ", ".join(missing))

    def __len__(self) -> int: return self.count

    def _text(self, rng: random.Random) -> tuple[str, str]:
        kind = rng.randrange(100)
        if kind < 40:
            # Parenthetical quality names occur on treasure/gems. Rarity is a
            # separate tooltip row and must not be invented as a title suffix.
            suffix = rng.choice(["", "", "", "", " (Cracked)", " (Flawed)",
                                 " (Fine)", " (Perfect)", " (Royal)"])
            return rng.choice(self.items) + suffix, "medium"
        if kind < 67:
            stat = rng.choice(self.stats)
            if rng.random() < .58:
                value = str(rng.randint(-50, 99))
            else:
                value = f"{rng.randint(1, 99)}.{rng.randint(0, 9)}%"
            prefix = "+" if kind >= 35 and not value.startswith("-") else ""
            line = f"{prefix}{value} {stat}" if prefix else f"{stat} {value}"
            # The detailed-stat view appends the item's possible roll range.
            # Include it in synthesis so [, ], and ~ are learned as normal
            # font glyphs rather than only appearing in a handful of captures.
            if rng.random() < .28:
                low, high = sorted(rng.sample(range(1, 51), 2))
                percent = "%" if "%" in value else ""
                line += f" [{low}{percent} ~ {high}{percent}]"
            return line, "light"
        if kind < 84:
            key = rng.choice(list(METADATA))
            return f"{key}: {rng.choice(METADATA[key])}", "light"
        if kind < 89:
            sample = rng.sample(CLASSES, rng.randint(1, 3))
            return ", ".join(sample), "light"
        if kind < 96:
            text = rng.choice(self.prose)
            return rng.choice(wrap_lines(text, rng)), "light"
        # Gold italic ownership footer. Mixed case and digits are especially
        # important here because player names do not resemble prose words.
        alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789"
        username = "".join(rng.choice(alphabet) for _ in range(rng.randint(5, 22)))
        verb = rng.choice(("Found", "Found", "Crafted"))
        return f'"{verb} by {username}"', "flavor"

    def __getitem__(self, index: int) -> tuple[torch.Tensor, str]:
        rng = random.Random(self.seed + index * 104729)
        text, face = self._text(rng)
        text = "".join(c for c in text if c in ASCII or c == " ").strip()
        size = rng.randint(20, 28) if face != "medium" else rng.randint(23, 31)
        font = ImageFont.truetype(str(self.fonts[face]), size=size)
        box = font.getbbox(text, stroke_width=0)
        w = max(8, box[2] - box[0] + rng.randint(8, 18))
        h = max(18, box[3] - box[1] + rng.randint(8, 14))
        bg = rng.randint(3, 24)
        image = Image.new("RGB", (w, h), (bg, bg, bg))
        draw = ImageDraw.Draw(image)
        color = rng.choice(COLORS)
        draw.text(((w - (box[2] - box[0])) // 2 - box[0],
                   (h - (box[3] - box[1])) // 2 - box[1]), text, font=font, fill=color)

        if rng.random() < .35: image = image.filter(ImageFilter.GaussianBlur(rng.uniform(0, .55)))
        if rng.random() < .25: image = ImageEnhance.Contrast(image).enhance(rng.uniform(.75, 1.25))

        gray = production_normalize(image).astype(np.float32)
        if rng.random() < .4:
            gray += np.random.default_rng(self.seed + index).normal(0, rng.uniform(.2, 2.2), gray.shape)
        normalized = np.clip(gray, 0, 255).astype(np.uint8)
        rw = min(WIDTH, max(1, int(np.floor(normalized.shape[1] * HEIGHT
                                             / normalized.shape[0] + .5))))
        normalized = cv2.resize(normalized, (rw, HEIGHT), interpolation=cv2.INTER_CUBIC)
        canvas = np.zeros((HEIGHT, WIDTH), dtype=np.float32)
        canvas[:, :rw] = normalized.astype(np.float32)
        tensor = torch.from_numpy(canvas).unsqueeze(0).repeat(3, 1, 1) / 127.5 - 1.0
        return tensor, text


class FooterLines(TooltipLines):
    """Pelagiad-only ownership rows with unconstrained player-name strings."""

    def _text(self, rng: random.Random) -> tuple[str, str]:
        alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789"
        username = "".join(rng.choice(alphabet) for _ in range(rng.randint(3, 24)))
        return f'"{rng.choice(("Found", "Found", "Crafted"))} by {username}"', "flavor"


class RealTooltipLines(Dataset):
    """Repeat and perturb labelled game captures for raster-domain adaptation."""

    def __init__(self, manifest: Path, crop_dir: Path, count: int, seed: int,
                 augment: bool = True) -> None:
        self.rows = [line.rstrip("\n").split("\t", 1)
                     for line in manifest.open(encoding="utf-8") if line.strip()]
        self.rows = [(crop_dir / name, text) for name, text in self.rows]
        missing = [str(path) for path, _ in self.rows if not path.exists()]
        if missing: raise FileNotFoundError("missing real crops: " + ", ".join(missing[:5]))
        self.count, self.seed, self.augment = count, seed, augment

    def __len__(self) -> int: return self.count

    def __getitem__(self, index: int) -> tuple[torch.Tensor, str]:
        rng = random.Random(self.seed + index * 65537)
        path, text = self.rows[index % len(self.rows)]
        image = Image.open(path)
        if path.name.endswith("_band0.png"):
            image = production_title_image(image)
        image = image.convert("L")
        if not self.augment:
            gray = production_normalize(image)
            rw = min(WIDTH, max(1, int(np.floor(gray.shape[1] * HEIGHT
                                                 / gray.shape[0] + .5))))
            gray = cv2.resize(gray, (rw, HEIGHT), interpolation=cv2.INTER_CUBIC)
            canvas = np.zeros((HEIGHT, WIDTH), dtype=np.float32)
            canvas[:, :rw] = gray.astype(np.float32)
            return (torch.from_numpy(canvas).unsqueeze(0).repeat(3, 1, 1)
                    / 127.5 - 1.0).float(), text
        # Vary the raster domain around Unreal's captured output: independent
        # horizontal scale, stroke weight, blur, gamma, and sensor noise.
        if rng.random() < .65:
            sx, sy = rng.uniform(.95, 1.05), rng.uniform(.96, 1.04)
            image = image.resize((max(2, round(image.width * sx)),
                                  max(2, round(image.height * sy))), Image.Resampling.BICUBIC)
        if rng.random() < .5:
            gray = production_normalize(image).astype(np.float32)
        else:
            gray = np.asarray(image, dtype=np.float32)
            histogram = np.bincount(gray.astype(np.uint8).ravel(), minlength=256)
            cumulative = np.cumsum(histogram)
            background = int(np.searchsorted(cumulative, gray.size // 2, side="left"))
            foreground = int(np.searchsorted(cumulative, gray.size * 49 // 50, side="left"))
            span = max(24.0, foreground - background)
            gamma = rng.uniform(.46, .56)
            gray = np.power(np.clip((gray - background) / span, 0, 1), gamma) * 255.0
        image = Image.fromarray(np.clip(gray, 0, 255).astype(np.uint8), "L")
        roll = rng.random()
        if roll < .04: image = image.filter(ImageFilter.MaxFilter(3))
        elif roll < .08: image = image.filter(ImageFilter.MinFilter(3))
        if rng.random() < .3: image = image.filter(ImageFilter.GaussianBlur(rng.uniform(0, .35)))
        gray = np.asarray(image, dtype=np.uint8)
        rw = min(WIDTH, max(1, int(np.floor(gray.shape[1] * HEIGHT
                                             / gray.shape[0] + .5))))
        gray = cv2.resize(gray, (rw, HEIGHT), interpolation=cv2.INTER_CUBIC)
        canvas = np.zeros((HEIGHT, WIDTH), dtype=np.float32)
        canvas[:, :rw] = gray.astype(np.float32)
        if rng.random() < .5:
            canvas += np.random.default_rng(self.seed + index).normal(0, rng.uniform(.1, 2.0), canvas.shape)
        tensor = torch.from_numpy(np.clip(canvas, 0, 255)).unsqueeze(0).repeat(3, 1, 1) / 127.5 - 1.0
        return tensor.float(), text
