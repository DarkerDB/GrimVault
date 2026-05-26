"""Generate a multi-resolution .ico from a square PNG.

Usage: python png_to_ico.py <input.png> <output.ico>

Emits sizes [16, 24, 32, 48, 64, 128, 256] — covers Explorer thumbnails,
taskbar, alt-tab, jump lists, and the NSIS installer header.
Requires Pillow (pip install pillow).
"""

import sys
from PIL import Image

SIZES = [ (16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256) ]

def main () -> int:
   if len (sys.argv) != 3:
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
   return 0

if __name__ == "__main__":
   sys.exit (main ())
