# models/

Runtime ONNX inference models. NOT committed to git (too large; binary; cycles independently of the source tree).

Expected layout at runtime:

```
models/
   tooltip.onnx                    YOLOv8n tooltip-region detector
   paddle/
      <family>/
         rec.onnx                  PaddleOCR recognizer (per script family)
         dict.txt                  Per-recognizer character dictionary
```

Where to get them:

1. **Existing Electron build** — copy from `src/native/.build/models/` if you have a working v1.x checkout.
2. **DarkerDB releases** — download the `models-vX.Y.Z.zip` artifact attached to any GrimVault GitHub Release and extract here.

Without these files, the tooltip detector and OCR pipeline log init failures at startup; the main UI still launches and the rest of the app remains usable. See `DEV.md` for the full dev-mode setup.
