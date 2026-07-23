#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
import json
import subprocess
import sys
import tempfile
from pathlib import Path

import yaml


def main() -> None:
    root = Path(__file__).resolve().parents[1]
    converter = root / "kalibr_to_basalt_unified_json.py"
    camchain = {
        "cam0": {
            "camera_model": "pinhole",
            "distortion_model": "equidistant",
            "intrinsics": [300.0, 301.0, 320.0, 240.0],
            "distortion_coeffs": [0.1, 0.01, 0.001, 0.0001],
            "resolution": [640, 480],
        },
        "cam1": {
            "camera_model": "pinhole",
            "distortion_model": "equidistant",
            "intrinsics": [302.0, 303.0, 321.0, 241.0],
            "distortion_coeffs": [0.2, 0.02, 0.002, 0.0002],
            "resolution": [640, 480],
            "T_cn_cnm1": [
                [1.0, 0.0, 0.0, -0.12],
                [0.0, 1.0, 0.0, 0.0],
                [0.0, 0.0, 1.0, 0.0],
                [0.0, 0.0, 0.0, 1.0],
            ],
        },
    }

    with tempfile.TemporaryDirectory() as tmp:
        tmp_path = Path(tmp)
        camchain_path = tmp_path / "camchain.yaml"
        out_path = tmp_path / "runtime.json"
        camchain_path.write_text(yaml.safe_dump(camchain), encoding="utf-8")
        subprocess.run(
            [
                sys.executable,
                str(converter),
                "--camchain",
                str(camchain_path),
                "--out",
                str(out_path),
                "--imu-update-rate",
                "200",
                "--accel-noise-density",
                "0.01",
                "--accel-random-walk",
                "0.001",
                "--gyro-noise-density",
                "0.001",
                "--gyro-random-walk",
                "0.0001",
                "--no-imu",
            ],
            check=True,
        )
        value = json.loads(out_path.read_text(encoding="utf-8"))["value0"]
        cam0 = value["T_imu_cam"][0]
        cam1 = value["T_imu_cam"][1]
        assert cam0 == {
            "px": 0.0,
            "py": 0.0,
            "pz": 0.0,
            "qx": 0.0,
            "qy": 0.0,
            "qz": 0.0,
            "qw": 1.0,
        }
        assert abs(cam1["px"] - 0.12) < 1e-12
        assert abs(cam1["py"]) < 1e-12
        assert abs(cam1["pz"]) < 1e-12
        assert value["cam_time_offset_ns"] == 0
        assert value["resolution"] == [[640, 480], [640, 480]]


if __name__ == "__main__":
    main()
