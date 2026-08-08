"""Generate GrimVault Windows icon and optional NSIS branding bitmaps.

Usage: python png_to_ico.py <input.png> <output.ico> [installer-assets-dir]

Emits sizes [16, 24, 32, 48, 64, 128, 256] — covers Explorer thumbnails,
taskbar, alt-tab, jump lists, and the NSIS installer header.
Requires Pillow (pip install pillow).
"""

import sys
from pathlib import Path

from PIL import Image

SIZES = [ (16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256) ]

def fit (source: Image.Image, size: tuple[int, int], margin: int) -> Image.Image:
   width, height = size
   canvas = Image.new ("RGBA", size, "#11100e")
   image = source.copy ()
   image.thumbnail ((width - margin * 2, height - margin * 2), Image.Resampling.LANCZOS)
   left = (width - image.width) // 2
   top = (height - image.height) // 2
   canvas.alpha_composite (image, (left, top))
   return canvas.convert ("RGB")


def main () -> int:
   if len (sys.argv) not in (3, 4):
      print (__doc__, file=sys.stderr)
      return 2

   src, dst = sys.argv[1], sys.argv[2]
   img = Image.open (src).convert ("RGBA")

   if img.width != img.height:
      side = min (img.width, img.height)
      left = (img.width  - side) // 2
      top  = (img.height - side) // 2
      img  = img.crop ((left, top, left + side, top + side))

   img.save (dst, format="ICO", sizes=SIZES)
   print (f"wrote {dst} ({len (SIZES)} sizes)")

   if len (sys.argv) == 4:
      assets = Path (sys.argv[3])
      assets.mkdir (parents=True, exist_ok=True)
      fit (img, (150, 57), 5).save (assets / "Header.bmp", format="BMP")
      fit (img, (164, 314), 15).save (assets / "Welcome.bmp", format="BMP")
      print (f"wrote {assets / 'Header.bmp'} and {assets / 'Welcome.bmp'}")

   return 0

if __name__ == "__main__":
   sys.exit (main ())
