from __future__ import annotations

import argparse
import json
from pathlib import Path

import torch
from torch import nn
from torch.utils.data import ConcatDataset, DataLoader

from dataset import ASCII, HEIGHT, WIDTH, FooterLines, RealTooltipLines, TooltipLines
from model import make_model


def collate(batch):
    images, texts = zip(*batch)
    lookup = {c: i + 1 for i, c in enumerate(ASCII)}
    space = len(ASCII) + 1
    encoded = [[space if c == " " else lookup[c] for c in text] for text in texts]
    lengths = torch.tensor([len(v) for v in encoded], dtype=torch.long)
    targets = torch.tensor([c for row in encoded for c in row], dtype=torch.long)
    return torch.stack(images), targets, lengths, texts


@torch.inference_mode()
def decode(logits: torch.Tensor) -> list[str]:
    result = []
    for sequence in logits.argmax(2).cpu().tolist():
        text, previous = "", -1
        for value in sequence:
            if value and value != previous:
                text += " " if value == len(ASCII) + 1 else ASCII[value - 1]
            previous = value
        result.append(text)
    return result


def main() -> None:
    here = Path(__file__).resolve().parent
    parser = argparse.ArgumentParser()
    parser.add_argument("--ddb", type=Path, default=Path.home()/".katforge/realms/darkerdb.com")
    parser.add_argument("--font-dir", type=Path, default=None)
    parser.add_argument("--output", type=Path, default=here/"output")
    parser.add_argument("--samples", type=int, default=120_000)
    parser.add_argument("--footer-samples", type=int, default=0)
    parser.add_argument("--epochs", type=int, default=8)
    parser.add_argument("--batch", type=int, default=96)
    parser.add_argument("--workers", type=int, default=8)
    parser.add_argument("--resume", type=Path)
    parser.add_argument("--lr", type=float, default=2e-3)
    parser.add_argument("--real-manifest", type=Path)
    parser.add_argument("--real-crop-dir", type=Path,
                        default=here/"real-crops")
    parser.add_argument("--valid-manifest", type=Path)
    parser.add_argument("--valid-crop-dir", type=Path)
    parser.add_argument("--real-samples", type=int, default=0)
    parser.add_argument("--focus-manifest", type=Path)
    parser.add_argument("--focus-crop-dir", type=Path)
    parser.add_argument("--focus-samples", type=int, default=0)
    parser.add_argument("--valid-samples", type=int, default=2_000)
    parser.add_argument("--freeze-bn", action="store_true",
                        help="keep running image statistics fixed during adaptation")
    parser.add_argument("--freeze-features", action="store_true",
                        help="adapt only the sequence/context head")
    parser.add_argument("--architecture", choices=("v1", "v2"), default="v1")
    args = parser.parse_args()
    if not args.font_dir:
        candidates = (
            args.ddb/"tooltips/dist/assets",
            Path.home()/".katforge/katforge.com/public/assets/fonts",
            Path.home()/".katforge/packages/spark/src/assets/fonts",
        )
        required = ("SaintKDG_Light.ttf", "SaintKDG_Medium.ttf", "Pelagiad.ttf")
        args.font_dir = next((path for path in candidates
                              if all((path/name).exists() for name in required)), candidates[0])
    args.output.mkdir(parents=True, exist_ok=True)

    device = torch.device("cuda" if torch.cuda.is_available() else "cpu")
    print(f"device={device} samples={args.samples} size={HEIGHT}x{WIDTH}", flush=True)
    datasets = []
    if args.samples:
        datasets.append(TooltipLines(args.samples, 0xDDB, args.ddb, args.font_dir))
    if args.footer_samples:
        datasets.append(FooterLines(args.footer_samples, 0xF007E2, args.ddb, args.font_dir))
    if args.real_manifest and args.real_samples:
        datasets.append(RealTooltipLines(
            args.real_manifest, args.real_crop_dir, args.real_samples, 0xA11CE))
    if args.focus_manifest and args.focus_samples:
        datasets.append(RealTooltipLines(
            args.focus_manifest, args.focus_crop_dir or args.real_crop_dir,
            args.focus_samples, 0xF0C05))
    if not datasets:
        parser.error("training requires synthetic, real, or focus samples")
    train = datasets[0] if len(datasets) == 1 else ConcatDataset(datasets)
    # A supplied real validation set is authoritative. Previously, enabling
    # synthetic training silently caused validation to use synthetic rows too,
    # which made the reported score useless for judging game captures.
    if (args.valid_manifest or args.real_manifest) and args.valid_samples:
        valid = RealTooltipLines(
            args.valid_manifest or args.real_manifest,
            args.valid_crop_dir or args.real_crop_dir,
            args.valid_samples, 0xC0FFEE, augment=False)
    elif args.samples and args.valid_samples:
        valid = TooltipLines(args.valid_samples, 0xC0FFEE, args.ddb, args.font_dir)
    else:
        parser.error("validation requires samples and a matching data source")
    train_loader = DataLoader(train, args.batch, shuffle=True, num_workers=args.workers,
                              pin_memory=device.type == "cuda", collate_fn=collate,
                              persistent_workers=args.workers > 0)
    valid_loader = DataLoader(valid, args.batch, num_workers=max(1, args.workers // 2),
                              collate_fn=collate)
    architecture = args.architecture
    if args.resume:
        state = torch.load(args.resume, map_location="cpu", weights_only=True)
        architecture = state.get("architecture", "v1")
    model = make_model(len(ASCII) + 2, architecture).to(device)
    if args.resume:
        model.load_state_dict(state["model"])
        print(f"resumed={args.resume} prior_accuracy={state.get('accuracy', 0):.4%}", flush=True)
    if args.freeze_features:
        for parameter in model.features.parameters():
            parameter.requires_grad_(False)

    @torch.inference_mode()
    def validation_accuracy() -> float:
        model.eval()
        correct = total = 0
        for images, _, _, texts in valid_loader:
            with torch.amp.autocast("cuda", enabled=device.type == "cuda"):
                predicted = decode(model(images.to(device)))
            correct += sum(a == b for a, b in zip(predicted, texts))
            total += len(texts)
        return correct / total

    optimizer = torch.optim.AdamW(model.parameters(), lr=args.lr, weight_decay=1e-4)
    scheduler = torch.optim.lr_scheduler.CosineAnnealingLR(optimizer, args.epochs)
    criterion = nn.CTCLoss(blank=0, zero_infinity=True)
    scaler = torch.amp.GradScaler("cuda", enabled=device.type == "cuda")
    best = -1.0
    if args.resume:
        best = validation_accuracy()
        torch.save({"model": model.state_dict(), "ascii": ASCII, "accuracy": best,
                    "architecture": architecture}, args.output/"best.pt")
        print(f"baseline_exact_accuracy={best:.4%}", flush=True)

    for epoch in range(1, args.epochs + 1):
        model.train()
        if args.freeze_features:
            model.features.eval()
        if args.freeze_bn:
            for module in model.modules():
                if isinstance(module, (nn.BatchNorm1d, nn.BatchNorm2d)):
                    module.eval()
        running = 0.0
        for step, (images, targets, lengths, _) in enumerate(train_loader, 1):
            images, targets = images.to(device, non_blocking=True), targets.to(device)
            optimizer.zero_grad(set_to_none=True)
            with torch.amp.autocast("cuda", enabled=device.type == "cuda"):
                logits = model(images)
                input_lengths = torch.full((images.size(0),), logits.size(1), dtype=torch.long)
                loss = criterion(logits.log_softmax(2).transpose(0, 1), targets,
                                 input_lengths, lengths)
            scaler.scale(loss).backward()
            scaler.unscale_(optimizer); nn.utils.clip_grad_norm_(model.parameters(), 5.0)
            scaler.step(optimizer); scaler.update(); running += loss.item()
            if step % 10 == 0:
                print(f"epoch={epoch} step={step}/{len(train_loader)} loss={running/10:.4f}", flush=True)
                running = 0.0
        scheduler.step()

        accuracy = validation_accuracy()
        print(f"epoch={epoch} exact_accuracy={accuracy:.4%}", flush=True)
        torch.save({"model": model.state_dict(), "ascii": ASCII, "accuracy": accuracy,
                    "architecture": architecture},
                   args.output/"latest.pt")
        if accuracy > best:
            best = accuracy
            torch.save({"model": model.state_dict(), "ascii": ASCII, "accuracy": accuracy,
                        "architecture": architecture},
                       args.output/"best.pt")

    (args.output/"metrics.json").write_text(json.dumps({
        "validation_exact": best, "architecture": architecture,
    }, indent=2))


if __name__ == "__main__": main()
