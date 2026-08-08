from __future__ import annotations

import argparse
from pathlib import Path

import cv2
import numpy as np
import onnxruntime as ort
import torch
from PIL import Image

from dataset import ASCII, HEIGHT, WIDTH, production_normalize, production_title_image
from model import make_model
from train import decode


def prepare(path: Path) -> torch.Tensor:
    image = Image.open(path)
    if path.name.endswith("_band0.png"):
        image = production_title_image(image)
    rgb = np.asarray(image.convert("RGB"))
    gray = cv2.cvtColor(rgb, cv2.COLOR_RGB2GRAY)
    histogram = np.bincount(gray.ravel(), minlength=256)
    cumulative = np.cumsum(histogram)
    background = int(np.searchsorted(cumulative, gray.size // 2, side="left"))
    foreground = int(np.searchsorted(cumulative, gray.size * 49 // 50, side="left"))
    span = max(24, foreground - background)
    values = np.arange(256, dtype=np.float64)
    lut = np.floor(255 * np.sqrt(np.clip((values - background) / span, 0, 1)) + .5).astype(np.uint8)
    gray = lut[gray]
    rw = min(WIDTH, max(1, int(np.floor(gray.shape[1] * HEIGHT / gray.shape[0] + .5))))
    gray = cv2.resize(gray, (rw, HEIGHT), interpolation=cv2.INTER_CUBIC)
    canvas = np.zeros((HEIGHT, WIDTH), dtype=np.float32)
    canvas[:, :rw] = gray.astype(np.float32)
    return torch.from_numpy(canvas).unsqueeze(0).repeat(3, 1, 1) / 127.5 - 1.0


def main() -> None:
    here = Path(__file__).resolve().parent
    parser = argparse.ArgumentParser()
    parser.add_argument("checkpoint", type=Path)
    parser.add_argument("--manifest", type=Path, default=here/"real-eval.tsv")
    parser.add_argument("--crop-dir", type=Path,
                        default=here/"real-crops")
    parser.add_argument("--backend", choices=("opencv", "onnxruntime"),
                        default="opencv",
                        help="ONNX inference backend; OpenCV matches production")
    args = parser.parse_args()
    rows = [line.rstrip("\n").split("\t", 1) for line in args.manifest.open(encoding="utf-8")]
    model = None
    session = None
    cv_net = None
    if args.checkpoint.suffix == ".onnx":
        if args.backend == "opencv":
            cv_net = cv2.dnn.readNetFromONNX(str(args.checkpoint))
            cv_net.setPreferableBackend(cv2.dnn.DNN_BACKEND_OPENCV)
            cv_net.setPreferableTarget(cv2.dnn.DNN_TARGET_CPU)
        else:
            session = ort.InferenceSession(str(args.checkpoint), providers=["CPUExecutionProvider"])
    else:
        state = torch.load(args.checkpoint, map_location="cpu", weights_only=True)
        model = make_model(len(ASCII) + 2, state.get("architecture", "v1")).eval()
        model.load_state_dict(state["model"])
    exact = 0
    for filename, expected in rows:
        path = args.crop_dir / filename
        if not path.exists(): continue
        image = prepare(path).unsqueeze(0)
        if cv_net is not None:
            cv_net.setInput(image.numpy())
            logits = torch.from_numpy(cv_net.forward())
        elif session is not None:
            logits = torch.from_numpy(session.run(None, {"x": image.numpy()})[0])
        else:
            logits = model(image)
        actual = decode(logits)[0]
        ok = actual == expected; exact += ok
        print(("PASS" if ok else "FAIL") + f" {filename}: {actual!r}")
        if not ok: print(f"     expected: {expected!r}")
    print(f"real_exact={exact}/{len(rows)} ({exact/len(rows):.2%})")


if __name__ == "__main__": main()
