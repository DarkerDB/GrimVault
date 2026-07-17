from __future__ import annotations

import argparse
import shutil
from pathlib import Path


def main() -> None:
    here = Path(__file__).resolve().parent
    parser = argparse.ArgumentParser(
        description="Preserve labelled debug bands before the next run reuses their sequence numbers")
    parser.add_argument("manifests", nargs="+", type=Path)
    parser.add_argument("--source", type=Path,
                        default=Path("/mnt/c/Users/Ethan/AppData/Local/Temp/grimvault-ocr"))
    parser.add_argument("--destination", type=Path, default=here/"real-crops")
    args = parser.parse_args()
    args.destination.mkdir(parents=True, exist_ok=True)
    copied: set[str] = set()
    for manifest in args.manifests:
        for line in manifest.open(encoding="utf-8"):
            if not line.strip(): continue
            filename = line.split("\t", 1)[0]
            if filename in copied: continue
            # Debug runs reuse numeric sequence names (0_band0.png, ...).
            # Manifests may namespace captures in subdirectories so importing a
            # later run cannot overwrite an older, differently labelled crop.
            source = args.source / Path(filename).name
            if not source.exists(): raise FileNotFoundError(source)
            destination = args.destination / filename
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source, destination)
            copied.add(filename)
    print(f"preserved {len(copied)} labelled crops in {args.destination}")


if __name__ == "__main__": main()
