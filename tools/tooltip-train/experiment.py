from pathlib import Path

import torch.nn as nn

from yolox.exp import Exp as BaseExp


class Exp(BaseExp):
    def __init__(self):
        super().__init__()
        root = Path(__file__).parent
        self.num_classes = 1
        self.depth = 0.33
        self.width = 0.25
        self.input_size = (416, 416)
        self.test_size = (416, 416)
        self.random_size = (10, 16)
        self.mosaic_scale = (0.5, 1.5)
        self.mosaic_prob = 0.5
        self.enable_mixup = False
        self.max_epoch = 100
        self.warmup_epochs = 2
        self.no_aug_epochs = 10
        self.eval_interval = 5
        self.data_num_workers = 4
        self.save_history_ckpt = False
        self.data_dir = str(root / "data")
        self.train_ann = "instances_train2017.json"
        self.val_ann = "instances_val2017.json"
        self.output_dir = str(root / "output")
        self.exp_name = "tooltip-nano"

    def get_model(self):
        from yolox.models import YOLOPAFPN, YOLOX, YOLOXHead

        def initialize(module):
            for layer in module.modules():
                if isinstance(layer, nn.BatchNorm2d):
                    layer.eps = 1e-3
                    layer.momentum = 0.03

        if getattr(self, "model", None) is None:
            channels = [256, 512, 1024]
            backbone = YOLOPAFPN(self.depth, self.width, in_channels=channels, act=self.act, depthwise=True)
            head = YOLOXHead(self.num_classes, self.width, in_channels=channels, act=self.act, depthwise=True)
            self.model = YOLOX(backbone, head)

        self.model.apply(initialize)
        self.model.head.initialize_biases(1e-2)
        return self.model
