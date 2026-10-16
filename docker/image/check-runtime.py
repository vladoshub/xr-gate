#!/usr/bin/env python3
"""Software-only image check; real camera/BLE operation needs hardware."""
import importlib
from pathlib import Path
import subprocess

root = Path("/opt/xr-gate")
for module in ("yaml", "numpy", "cv2"):
    importlib.import_module(module)
    print(f"OK Python {module}")
subprocess.run(["python3", str(root / "devices/common/linux/scripts/runtime/cpu_target.py"), "check", str(root)], check=True)
adapter = root / "bin/runtime_adapters/xr_runtime_adapter/xr_runtime_adapter"
help_text = subprocess.check_output([str(adapter), "--help"], text=True)
assert "--app-tcp" in help_text, "Release does not support application TCP"
for name in ("grayscale_detection_160x160.onnx", "grayscale_keypoint_jan18.onnx"):
    assert (root / "bin/hand-tracking-models/mercury" / name).is_file(), name
print("OK release binary, TCP option, CPU target, Mercury models")
