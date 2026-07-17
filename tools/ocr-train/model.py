from __future__ import annotations

import torch
from torch import nn


class FontCtc(nn.Module):
    """Small OpenCV-DNN-friendly fully-convolutional line recognizer."""

    def __init__(self, classes: int) -> None:
        super().__init__()
        self.features = nn.Sequential(
            nn.Conv2d(3, 16, 3, stride=2, padding=1), nn.BatchNorm2d(16), nn.ReLU(),
            nn.Conv2d(16, 16, 3, padding=1, groups=16), nn.BatchNorm2d(16), nn.ReLU(),
            nn.Conv2d(16, 32, 1), nn.BatchNorm2d(32), nn.ReLU(),  # 24 x W/2
            nn.MaxPool2d(2, 2),                                  # 12 x W/4
            nn.Conv2d(32, 32, 3, padding=1, groups=32), nn.BatchNorm2d(32), nn.ReLU(),
            nn.Conv2d(32, 64, 1), nn.BatchNorm2d(64), nn.ReLU(),
            nn.MaxPool2d(2, 2),                                  # 6 x W/8
            nn.Conv2d(64, 64, 3, padding=1, groups=64), nn.BatchNorm2d(64), nn.ReLU(),
            nn.Conv2d(64, 96, 1), nn.BatchNorm2d(96), nn.ReLU(),
            nn.MaxPool2d((6, 1), (6, 1)),                         # 1 x W/8
        )
        self.context = nn.Sequential(
            nn.Conv1d(96, 128, 5, padding=2), nn.BatchNorm1d(128), nn.ReLU(),
            nn.Conv1d(128, classes, 1),
        )

    def forward(self, image: torch.Tensor) -> torch.Tensor:
        x = self.features(image).squeeze(2)
        return self.context(x).transpose(1, 2)          # B,T,C


class FontCtcV2(nn.Module):
    """Higher-capacity recognizer for mixed real/synthetic game-font domains."""

    def __init__(self, classes: int) -> None:
        super().__init__()
        self.features = nn.Sequential(
            nn.Conv2d(3, 24, 3, stride=2, padding=1), nn.BatchNorm2d(24), nn.ReLU(),
            nn.Conv2d(24, 32, 3, padding=1), nn.BatchNorm2d(32), nn.ReLU(),
            nn.MaxPool2d(2, 2),                                  # 12 x W/4
            nn.Conv2d(32, 64, 3, padding=1), nn.BatchNorm2d(64), nn.ReLU(),
            nn.Conv2d(64, 64, 3, padding=1), nn.BatchNorm2d(64), nn.ReLU(),
            nn.MaxPool2d(2, 2),                                  # 6 x W/8
            nn.Conv2d(64, 128, 3, padding=1), nn.BatchNorm2d(128), nn.ReLU(),
            nn.Conv2d(128, 128, 3, padding=1), nn.BatchNorm2d(128), nn.ReLU(),
            nn.MaxPool2d((2, 1), (2, 1)),                         # 3 x W/8
            nn.Conv2d(128, 160, 3, padding=1), nn.BatchNorm2d(160), nn.ReLU(),
            nn.MaxPool2d((3, 1), (3, 1)),                         # 1 x W/8
        )
        self.context = nn.Sequential(
            nn.Conv1d(160, 256, 5, padding=2), nn.BatchNorm1d(256), nn.ReLU(),
            nn.Conv1d(256, 256, 5, padding=4, dilation=2, groups=256),
            nn.Conv1d(256, 256, 1), nn.BatchNorm1d(256), nn.ReLU(),
            nn.Conv1d(256, classes, 1),
        )

    def forward(self, image: torch.Tensor) -> torch.Tensor:
        x = self.features(image).squeeze(2)
        return self.context(x).transpose(1, 2)


def make_model(classes: int, architecture: str = "v1") -> nn.Module:
    if architecture == "v1":
        return FontCtc(classes)
    if architecture == "v2":
        return FontCtcV2(classes)
    raise ValueError(f"unknown architecture: {architecture}")
