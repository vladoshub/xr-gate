#!/usr/bin/env python3
"""Export the pinned FRAME stereo backbone for the XR Gate CPU backend."""

import argparse
import pathlib
import sys
from types import ModuleType

from hydra.utils import instantiate
import onnx
from omegaconf import OmegaConf
import torch


class StereoBackbone(torch.nn.Module):
    def __init__(self, backbone):
        super().__init__()
        self.backbone = backbone

    def forward(self, images, K, d):
        result = self.backbone(images, K, d)
        return result["joints_2D_norm"], result["joints_3D_cc"]


def load_backbone(checkpoint_path, source_dir):
    # Import the inference modules without loading FRAME's training or data tools.
    package = ModuleType("framevision")
    package.__path__ = [str(source_dir / "src" / "framevision")]
    sys.modules[package.__name__] = package

    # Python 3.13 serialized pathlib objects under this private module name.
    sys.modules.setdefault("pathlib._local", pathlib)
    # The build script verifies the pinned checkpoint before loading its Hydra config.
    checkpoint = torch.load(checkpoint_path, map_location="cpu", weights_only=False)
    network = checkpoint["hyper_parameters"]["hydra"]["model"]["network"]
    config = OmegaConf.to_container(network, resolve=True) if OmegaConf.is_config(network) else network
    config["encoder"]["name"] = config["encoder"]["name"].removeprefix("timm/")
    config["encoder"]["pretrained"] = False
    backbone = instantiate(config)
    state = {key.removeprefix("network.").replace("_orig_mod.", ""): value
             for key, value in checkpoint["state_dict"].items() if key.startswith("network.")}
    backbone.load_state_dict(state, strict=True)
    return StereoBackbone(backbone).eval()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", required=True, type=pathlib.Path)
    parser.add_argument("--checkpoint", required=True, type=pathlib.Path)
    parser.add_argument("--output", required=True, type=pathlib.Path)
    args = parser.parse_args()

    torch.set_num_threads(2)
    backbone = load_backbone(args.checkpoint, args.source)
    size = 256
    focal = 82.0 / size
    center = (size - 1) * 0.5 / size
    images = torch.zeros((1, 1, 2, 3, size, size), dtype=torch.float32)
    K = torch.tensor([[focal, 0, center], [0, focal, center], [0, 0, 1]], dtype=torch.float32)
    K = K.repeat(1, 2, 1, 1)
    d = torch.zeros((1, 2, 4), dtype=torch.float32)
    with torch.no_grad():
        torch.onnx.export(
            backbone, (images, K, d), str(args.output),
            input_names=["images", "K", "d"], output_names=["uv", "xyz"],
            opset_version=17, dynamo=False, external_data=False,
        )

    model = onnx.load(args.output)
    onnx.checker.check_model(model)
    # Keep the model loadable by the ONNX Runtime 1.18 SDK used by the backend.
    if model.ir_version > 10:
        raise ValueError(f"Unsupported ONNX IR version for FRAME: {model.ir_version}")
    expected = {"images": (1, 1, 2, 3, size, size), "K": (1, 2, 3, 3), "d": (1, 2, 4),
                "uv": (1, 1, 2, 15, 2), "xyz": (1, 1, 2, 15, 3)}
    for value in list(model.graph.input) + list(model.graph.output):
        tensor = value.type.tensor_type
        shape = tuple(dim.dim_value for dim in tensor.shape.dim)
        if tensor.elem_type != onnx.TensorProto.FLOAT or shape != expected.pop(value.name):
            raise ValueError(f"Unexpected FRAME tensor {value.name} with shape {shape}")
    if expected:
        raise ValueError(f"Missing FRAME tensors: {', '.join(expected)}")


if __name__ == "__main__":
    main()
