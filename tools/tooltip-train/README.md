# Tooltip detector training

This workflow trains the shipped 416×416 YOLOX-Nano detector from the recovered GrimVault tooltip screenshots. The source images and annotations in `data/` are the reproducible training set.

```powershell
uv sync --project tools/tooltip-train
uv run --project tools/tooltip-train python tools/tooltip-train/train.py
```

Use `--export-only` to rebuild the ONNX file from the selected checkpoint.

`prepare.py` recreates the compressed COCO dataset from the archived YOLO labels:

```powershell
uv run --project tools/tooltip-train python tools/tooltip-train/prepare.py C:\Users\Ethan\Documents\Projects\grimvault\training\data
```

The split is deterministic and keeps short adjacent capture sequences together. Training downloads the pinned Apache-2.0 YOLOX 0.3.0 source and official Nano checkpoint, verifies both checksums, fine-tunes for one tooltip class, validates on the held-out split, and writes `models/tooltip.onnx`.

CUDA is used only to train the model. GrimVault deploys it through DirectML with a CPU fallback.
