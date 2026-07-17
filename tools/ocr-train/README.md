# GrimVault font OCR training

This directory trains a compact convolutional CTC line recognizer against the
actual Dark and Darker tooltip faces (`SaintKDG_Light`, `SaintKDG_Medium`, and
`Pelagiad`). Font files are deliberately not copied into this public repository.

The generator reads localized names from a patch-pinned DDB API cache, then
renders deterministic synthetic tooltip lines with game colors, sizes, capture
noise, blur, and contrast variation. Spaces and punctuation are model outputs;
the runtime does not need character-guessing rules.

```bash
cd tools/ocr-train
uv sync --python 3.11
uv run python train.py
uv run python evaluate.py output/best.pt
uv run python export.py output/best.pt ../../models/paddle/en/rec_font.onnx
```

To adapt FreeType synthesis to Unreal's actual rasterization, label debug bands
in a tab-separated manifest and mix repeated, augmented real lines into a short
low-learning-rate continuation:

```bash
uv run python import_crops.py real-train.tsv real-holdout.tsv
uv run python train.py --resume output/best.pt --lr 0.0002 --epochs 5 \
  --samples 20000 --real-manifest real-train.tsv --real-samples 20000
```

Import labelled bands before restarting GrimVault: debug dump sequence numbers
restart at zero and may overwrite `%TEMP%/grimvault-ocr`. Put the filenames in a
per-run subdirectory in the manifest (for example,
`20260713-1509/0_band0.png`); the importer reads the basename from the debug
directory and preserves it under that namespace. `real-crops/` is local and
git-ignored.

The default training run uses CUDA when available. Generated environments,
API caches, checkpoints, frozen inbox datasets, and locally labelled crops are
ignored by git.

The exported model keeps the current runtime contract: input `1x3x48x960`,
output `1x120x96`, class zero as CTC blank, printable ASCII in `font_dict.txt`, and
space as the final class.

## Persistent sample inbox

Debug runs persist only object-detector-selected tooltip regions and their OCR
line crops under `%LocalAppData%/GrimVault/ocr-samples/inbox`. Each collision-
proof sample directory contains `tooltip.png`, `line-NN.png`, and
`metadata.json` with the unverified prediction, confidence, detector rectangle,
line ordering, and exact pixel hashes. Full game frames are never stored.

Predictions are deliberately not inserted into the training manifest. Build a
non-destructive, exact- and near-duplicate-aware review sheet from WSL with:

```bash
cd tools/ocr-train
.venv/bin/python review_inbox.py --output inbox-review.tsv
```

The least-confident occurrence of each exact pixel crop is retained for human
review. Near-duplicate clusters are advisory and are not deleted automatically;
color, rasterization, icon, and contrast variants may be valuable training data.

## Reproducible current-patch dataset

Fetch authoritative current-patch labels without copying the publishable API
key into generated files, freeze exact-unique tooltip crops, and partition them
by canonical item title:

```bash
cd tools/ocr-train
.venv/bin/python fetch_ddb_catalog.py
.venv/bin/python fetch_inbox_details.py
.venv/bin/python prepare_inbox_dataset.py
```

`prepare_inbox_dataset.py` writes verified-only `train.tsv`, `valid.tsv`, and
`test.tsv`. All captures for the same canonical item go to one split, preventing
same-item leakage. Uncertain predictions remain in `labels.jsonl` but are not
trainable. Directly inspected exceptions belong in the local `manual.tsv` as
`file<TAB>label`; `__SKIP__` excludes an unusable crop. Never hand-edit the
generated manifests.

The v2 model is a larger OpenCV-compatible CTC recognizer intended for a mixed
synthetic/real corpus. A representative run is:

```bash
.venv/bin/python train.py --architecture v2 --output output/robust \
  --samples 100000 \
  --real-manifest inbox-dataset-20260716/train.tsv \
  --real-crop-dir inbox-dataset-20260716/crops --real-samples 40000 \
  --focus-manifest real-hard.tsv --focus-crop-dir real-crops --focus-samples 10000 \
  --valid-manifest inbox-dataset-20260716/valid.tsv \
  --valid-crop-dir inbox-dataset-20260716/crops --valid-samples 296
```

For a newly trained mixed-domain checkpoint, BatchNorm statistics can be
re-estimated from clean training crops without gradient updates:

```bash
.venv/bin/python calibrate_bn.py output/robust/best.pt output/robust/calibrated.pt \
  --manifest inbox-dataset-20260716/train.tsv \
  --crop-dir inbox-dataset-20260716/crops
```

Model selection uses validation only. Evaluate the frozen test manifest once
after choosing a candidate, then require the full historical `real-train.tsv`
regression and exported OpenCV ONNX evaluation to pass before staging.
