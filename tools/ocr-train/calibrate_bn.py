from __future__ import annotations

import argparse
from pathlib import Path

import torch
from torch import nn
from torch.utils.data import DataLoader

from dataset import ASCII, RealTooltipLines
from model import make_model


def main() -> None:
    parser = argparse.ArgumentParser(description="Re-estimate BatchNorm statistics on clean game crops")
    parser.add_argument("checkpoint", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--crop-dir", type=Path, required=True)
    parser.add_argument("--batch", type=int, default=128)
    parser.add_argument("--workers", type=int, default=4)
    parser.add_argument("--passes", type=int, default=5)
    args = parser.parse_args()

    state = torch.load(args.checkpoint, map_location="cpu", weights_only=True)
    model = make_model(len(ASCII) + 2, state.get("architecture", "v1"))
    model.load_state_dict(state["model"])
    batch_norms = [module for module in model.modules()
                   if isinstance(module, (nn.BatchNorm1d, nn.BatchNorm2d))]
    for module in batch_norms:
        module.reset_running_stats()
        module.momentum = None

    device = torch.device("cuda" if torch.cuda.is_available() else "cpu")
    rows = sum(1 for line in args.manifest.open(encoding="utf-8") if line.strip())
    data = RealTooltipLines(args.manifest, args.crop_dir, rows, 0xCA11B, augment=False)
    loader = DataLoader(data, args.batch, shuffle=True, num_workers=args.workers,
                        pin_memory=device.type == "cuda")
    model.to(device).train()
    with torch.inference_mode():
        for _ in range(args.passes):
            for images, _ in loader:
                model(images.to(device, non_blocking=True))

    state["model"] = model.cpu().state_dict()
    state["bn_calibration"] = {"rows": rows, "passes": args.passes}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    torch.save(state, args.output)
    print(f"calibrated {len(batch_norms)} BatchNorm layers over {rows} rows x {args.passes}")


if __name__ == "__main__":
    main()
