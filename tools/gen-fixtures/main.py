#!/usr/bin/env python3
"""
Generate synthetic tooltip fixtures for the GrimVault E2E test suite.

Each fixture is a PNG that resembles a Dark and Darker tooltip: a centered
parchment-colored panel with the item name in white, a thin border, and
basic stat lines. Real game fixtures (recorded from actual gameplay) should
replace these once we have curated captures — but synthetic frames are
enough to exercise the WGC-FakeStrategy round-trip and to validate that
the YOLO tooltip detector finds *something* per (resolution × mode) cell.

Usage:
   python3 tools/gen-fixtures/main.py --out tests/fixtures
   python3 tools/gen-fixtures/main.py --out tests/fixtures --modes normal high_roller

Requires Pillow (pip install Pillow).
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

try:
   from PIL import Image, ImageDraw, ImageFont
except ImportError:
   sys.exit ("Pillow not installed. pip install Pillow")

RESOLUTIONS = [
   ("1920x1080", (1920, 1080)),
   ("2560x1440", (2560, 1440)),
   ("3840x2160", (3840, 2160)),
]

MODES = [ "normal", "high_roller", "ruins", "inferno", "goblin_caves" ]

ITEMS = [
   ("Sword of Truth",  "Legendary", "Damage: 45-60",  "Speed: 1.2"),
   ("Iron Dagger",     "Common",    "Damage: 12-18",  "Speed: 1.7"),
   ("Adventurer Robe", "Uncommon",  "Armor: 24",      "Magic Resist: +12"),
]

RARITY_COLORS = {
   "Common":     (238, 238, 238),
   "Uncommon":   (128, 214,   0),
   "Rare":       (  0, 170, 238),
   "Epic":       (208, 103, 255),
   "Legendary":  (255, 154,   0),
   "Unique":     (236, 217, 154),
   "Artifact":   (230,   5,   5),
}


def draw_tooltip (image, x, y, item):
   draw = ImageDraw.Draw (image)
   name, rarity, primary, secondary = item

   w, h = 320, 180

   draw.rectangle (
      [(x, y), (x + w, y + h)],
      fill    = (40, 30, 25, 220),
      outline = (180, 130, 80),
      width   = 3,
   )

   try:
      title_font = ImageFont.truetype ("DejaVuSans-Bold.ttf", 22)
      body_font  = ImageFont.truetype ("DejaVuSans.ttf",      16)
   except OSError:
      title_font = ImageFont.load_default ()
      body_font  = ImageFont.load_default ()

   draw.text ((x + w // 2, y + 16), name, fill=RARITY_COLORS.get (rarity, (255, 255, 255)),
              font=title_font, anchor="mt")
   draw.line ((x + 16, y + 50, x + w - 16, y + 50), fill=(180, 130, 80))
   draw.text ((x + 20, y + 64),  primary,   fill=(0, 170, 238), font=body_font)
   draw.text ((x + 20, y + 92),  secondary, fill=(0, 170, 238), font=body_font)
   draw.text ((x + 20, y + 132), "Powered by DarkerDB.com", fill=(150, 150, 150), font=body_font)


def make_frame (resolution, item):
   img = Image.new ("RGBA", resolution, (12, 12, 18, 255))

   draw = ImageDraw.Draw (img)
   for k in range (0, resolution [0], 40):
      draw.line ([(k, 0), (k, resolution [1])], fill=(20, 20, 30), width=1)

   center_x = (resolution [0] - 320) // 2
   center_y = (resolution [1] - 180) // 2
   draw_tooltip (img, center_x, center_y, item)
   return img


def main ():
   ap = argparse.ArgumentParser ()
   ap.add_argument ("--out",         required=True, type=Path)
   ap.add_argument ("--modes",       nargs="*", default=MODES)
   ap.add_argument ("--resolutions", nargs="*", default=[ r for r, _ in RESOLUTIONS ])
   args = ap.parse_args ()

   manifest = []

   for mode in args.modes:
      for res_name, res_size in RESOLUTIONS:
         if res_name not in args.resolutions: continue

         out_dir = args.out / mode / res_name
         out_dir.mkdir (parents=True, exist_ok=True)

         for i, item in enumerate (ITEMS):
            img      = make_frame (res_size, item)
            fname    = f"{i:02d}_{item [0].lower ().replace (' ', '_')}.png"
            fpath    = out_dir / fname
            img.save (fpath, "PNG")

            expected = {
               "name":    item [0],
               "rarity":  item [1],
               "primary": item [2],
               "secondary": item [3],
            }
            (out_dir / fname.replace (".png", ".json")).write_text (
               json.dumps (expected, indent=2)
            )

            manifest.append ({
               "mode": mode, "resolution": res_name, "file": str (fpath.relative_to (args.out)),
               **expected,
            })
            print (f"  {fpath.relative_to (args.out)}")

   (args.out / "manifest.json").write_text (json.dumps (manifest, indent=2))
   print (f"\nGenerated {len (manifest)} fixtures under {args.out}")
   print (f"Manifest: {args.out / 'manifest.json'}")


if __name__ == "__main__":
   main ()
