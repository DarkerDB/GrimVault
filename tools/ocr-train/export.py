from __future__ import annotations

import argparse
from pathlib import Path

import onnx
import torch
from torch import nn

from dataset import ASCII, HEIGHT, WIDTH
from model import make_model


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("checkpoint", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    state = torch.load(args.checkpoint, map_location="cpu", weights_only=True)
    if state["ascii"] != ASCII: raise RuntimeError("checkpoint dictionary mismatch")
    model = make_model(len(ASCII) + 2, state.get("architecture", "v1")).eval()
    model.load_state_dict(state["model"])
    args.output.parent.mkdir(parents=True, exist_ok=True)
    exported = nn.Sequential(model, nn.Softmax(dim=2))
    torch.onnx.export(exported, torch.zeros(1, 3, HEIGHT, WIDTH), args.output,
                      input_names=["x"], output_names=["output"], opset_version=17,
                      dynamo=False)
    onnx.checker.check_model(onnx.load(args.output))
    args.output.with_name("font_dict.txt").write_text(
        "\n".join(ASCII) + "\n", encoding="utf-8")
    print(f"exported {args.output} ({args.output.stat().st_size / 1e6:.2f} MB)")


if __name__ == "__main__": main()
