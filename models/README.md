# models/

Runtime ONNX inference models.

Expected layout at runtime:

```
models/
   tooltip-yolox-nano-416.onnx     Tooltip-region detector
   tooltip-yolox-nano.LICENSE.txt  Apache-2.0 text + provenance for the detector
   paddle/
      <family>/                    latin | eslav | korean | ch
         rec-ppocr-48x320.onnx          stock PaddleOCR recognizer
         rec-ppocr-48x320.dict.txt      its character set
         rec-tooltip-body-48x960.onnx   ours, trained on the game's faces
         rec-tooltip-48x960.dict.txt    its generated character set
      en/
         rec-ppocr-48x960.onnx          stock PaddleOCR, wide English line
         rec-ppocr-48x960.dict.txt      its character set
         rec-tooltip-body-48x960.onnx   ours, trained on the game's faces
         rec-tooltip-title-48x960.onnx  ours, title lines
         rec-tooltip-48x960.dict.txt    printable ASCII, shared by both
```

GrimVault prefers `rec-tooltip-body-48x960.onnx` when it and its dictionary are
present, and falls back to stock PaddleOCR otherwise.

A recognizer's name carries who trained it and the line geometry it expects —
`ppocr` is upstream PaddleOCR on the family character set, `tooltip` is ours on
a generated current-game character set. A dictionary is named for its model rather than
its directory, so a family holding two character sets cannot mispair them; body
and title share one because they share a charset. `gv::ocr::model_files`
(`include/gv/ocr/language.h`) is the only place these strings live.

## These are artifacts, not sources

GrimVault commits trained models and their redistributable runtime assets. Raw
training corpora, checkpoints, and internal training infrastructure are not
part of this public repository.

**No reduced model ships today.** The client looks for
`tooltip-yolox-nano-320.onnx` and quietly keeps the 416 model when it is
absent, which is the current state: a 320 re-export of a 416-trained
checkpoint localises too loosely for `TooltipTracker::refine`, which only
searches 20 px around each coarse edge. Measured on the held-out split it
lands inside that margin 19% of the time against the shipped model's 100%.
The internal detector evaluation records the measurements and remediation.

Filenames carry architecture and input edge because several `.onnx` files sit
side by side here. The rest of the run, including depth, width, epochs, and corpus size,
is ONNX metadata, readable with
`python -c "import onnx;print(onnx.load(P).metadata_props)"`.

Where to get the OCR models:

1. **Existing Electron build** — copy from `src/native/.build/models/` if you have a working v1.x checkout.
2. **DarkerDB releases** — download the `models-vX.Y.Z.zip` artifact attached to any GrimVault GitHub Release and extract here.

Without these files, the tooltip detector and OCR pipeline log init failures at startup; the main UI still launches and the rest of the app remains usable. See `DEV.md` for the full dev-mode setup.
