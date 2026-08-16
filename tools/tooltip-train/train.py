from __future__ import annotations

import argparse
import hashlib
import os
import shutil
import subprocess
import sys
import tarfile
import tempfile
import urllib.request
from pathlib import Path

import onnx


YOLOX_URL = "https://github.com/Megvii-BaseDetection/YOLOX/archive/refs/tags/0.3.0.tar.gz"
YOLOX_SHA256 = "972ddb9cb13d508fac3738e814449327cec29bc55fbd8a125c67d9080edfc02d"
WEIGHTS_URL = "https://github.com/Megvii-BaseDetection/YOLOX/releases/download/0.1.1rc0/yolox_nano.pth"
WEIGHTS_SHA256 = "cd28f55fbbc1829f99d9ac9b38a16d259a22889739c8728ea877610201feff7b"


def download(url: str, destination: Path, expected: str) -> None:
    urllib.request.urlretrieve(url, destination)
    actual = hashlib.sha256(destination.read_bytes()).hexdigest()
    if actual != expected:
        raise ValueError(f"checksum mismatch for {url}: {actual}")


def run(command: list[str], source: Path) -> None:
    environment = os.environ.copy()
    environment["PYTHONPATH"] = str(source)
    subprocess.run(command, cwd=source, env=environment, check=True)


def add_metadata(path: Path) -> None:
    model = onnx.load(path)
    metadata = {
        "architecture": "YOLOX-Nano",
        "input_size": "416x416",
        "license": "Apache-2.0",
        "source": "https://github.com/Megvii-BaseDetection/YOLOX/tree/0.3.0",
        "training_data": "GrimVault tooltip screenshots",
    }
    del model.metadata_props[:]
    for key, value in metadata.items():
        entry = model.metadata_props.add()
        entry.key = key
        entry.value = value
    onnx.checker.check_model(model)
    onnx.save(model, path)


def patch_source(source: Path) -> None:
    path = source / "yolox" / "data" / "datasets" / "coco.py"
    text = path.read_text(encoding="utf-8")
    text = text.replace('        dataset.pop("info", None)\n', "")
    text = text.replace('        dataset.pop("licenses", None)\n', "")
    path.write_text(text, encoding="utf-8")
    path = source / "tools" / "export_onnx.py"
    text = path.read_text(encoding="utf-8").replace("torch.onnx._export(", "torch.onnx.export(")
    text = text.replace("@logger.catch\n", "@logger.catch(reraise=True)\n")
    path.write_text(text, encoding="utf-8")
    path = source / "yolox" / "core" / "launch.py"
    text = path.read_text(encoding="utf-8")
    text = text.replace("@logger.catch\n", "@logger.catch(reraise=True)\n")
    path.write_text(text, encoding="utf-8")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--export-only", action="store_true")
    args = parser.parse_args()
    root = Path(__file__).resolve().parent
    if not (root / "data" / "annotations" / "instances_train2017.json").exists():
        raise FileNotFoundError("run prepare.py before training")

    checkpoint_dir = root / "output" / "tooltip-nano"
    checkpoint = checkpoint_dir / "best_ckpt.pth"
    if args.export_only and not checkpoint.exists():
        raise FileNotFoundError(checkpoint)
    if not args.export_only:
        shutil.rmtree(checkpoint_dir, ignore_errors=True)

    with tempfile.TemporaryDirectory(prefix="grimvault-tooltip-") as raw:
        temporary = Path(raw)
        archive = temporary / "yolox.tar.gz"
        download(YOLOX_URL, archive, YOLOX_SHA256)
        with tarfile.open(archive, "r:gz") as source_archive:
            source_archive.extractall(temporary, filter="data")
        source = temporary / "YOLOX-0.3.0"
        patch_source(source)
        if not args.export_only:
            weights = temporary / "yolox_nano.pth"
            download(WEIGHTS_URL, weights, WEIGHTS_SHA256)
            run([
                sys.executable,
                "tools/train.py",
                "-f", str(root / "experiment.py"),
                "-d", "1",
                "-b", "32",
                "--fp16",
                "-o",
                "-c", str(weights),
            ], source)

        if not checkpoint.exists():
            raise RuntimeError("training did not produce a validated checkpoint")

        output = root.parents[1] / "models" / "tooltip.onnx"
        exported = temporary / "tooltip.onnx"
        run([
            sys.executable,
            "tools/export_onnx.py",
            "-f", str(root / "experiment.py"),
            "-c", str(checkpoint),
            "--output-name", str(exported),
            "--decode_in_inference",
            "--opset", "17",
        ], source)
        if not exported.exists():
            raise RuntimeError("export did not produce a model")
        add_metadata(exported)
        shutil.copy2(exported, output)
        print(output)


if __name__ == "__main__":
    main()
