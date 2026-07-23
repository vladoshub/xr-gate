#!/usr/bin/env python3
import argparse
import json
import math
from pathlib import Path
from typing import Any, Dict, List

import yaml


def positive_float(value: str) -> float:
    number = float(value)
    if not math.isfinite(number) or number <= 0.0:
        raise argparse.ArgumentTypeError(
            f"expected a finite value greater than zero, got {value!r}"
        )
    return number


def nonnegative_float(value: str) -> float:
    number = float(value)
    if not math.isfinite(number) or number < 0.0:
        raise argparse.ArgumentTypeError(
            f"expected a finite non-negative value, got {value!r}"
        )
    return number


def mat_inv(T: List[List[float]]) -> List[List[float]]:
    R = [row[:3] for row in T[:3]]
    t = [T[0][3], T[1][3], T[2][3]]

    Rt = [[R[j][i] for j in range(3)] for i in range(3)]
    ti = [-sum(Rt[i][j] * t[j] for j in range(3)) for i in range(3)]

    return [
        [Rt[0][0], Rt[0][1], Rt[0][2], ti[0]],
        [Rt[1][0], Rt[1][1], Rt[1][2], ti[1]],
        [Rt[2][0], Rt[2][1], Rt[2][2], ti[2]],
        [0.0, 0.0, 0.0, 1.0],
    ]


def mat_to_quat(R: List[List[float]]):
    m00, m01, m02 = R[0]
    m10, m11, m12 = R[1]
    m20, m21, m22 = R[2]
    tr = m00 + m11 + m22

    if tr > 0:
        s = math.sqrt(tr + 1.0) * 2.0
        qw = 0.25 * s
        qx = (m21 - m12) / s
        qy = (m02 - m20) / s
        qz = (m10 - m01) / s
    elif m00 > m11 and m00 > m22:
        s = math.sqrt(1.0 + m00 - m11 - m22) * 2.0
        qw = (m21 - m12) / s
        qx = 0.25 * s
        qy = (m01 + m10) / s
        qz = (m02 + m20) / s
    elif m11 > m22:
        s = math.sqrt(1.0 + m11 - m00 - m22) * 2.0
        qw = (m02 - m20) / s
        qx = (m01 + m10) / s
        qy = 0.25 * s
        qz = (m12 + m21) / s
    else:
        s = math.sqrt(1.0 + m22 - m00 - m11) * 2.0
        qw = (m10 - m01) / s
        qx = (m02 + m20) / s
        qy = (m12 + m21) / s
        qz = 0.25 * s

    n = math.sqrt(qx * qx + qy * qy + qz * qz + qw * qw)
    if not math.isfinite(n) or n <= 0.0:
        raise ValueError("invalid rotation matrix: quaternion norm is zero or non-finite")
    return qx / n, qy / n, qz / n, qw / n


def pose_from_T(T: List[List[float]]) -> Dict[str, float]:
    qx, qy, qz, qw = mat_to_quat([row[:3] for row in T[:3]])
    return {
        "px": float(T[0][3]),
        "py": float(T[1][3]),
        "pz": float(T[2][3]),
        "qx": float(qx),
        "qy": float(qy),
        "qz": float(qz),
        "qw": float(qw),
    }


def require_matrix4(name: str, value: Any) -> List[List[float]]:
    if not isinstance(value, list) or len(value) != 4:
        raise ValueError(f"{name} must be a 4x4 matrix")
    matrix = []
    for row in value:
        if not isinstance(row, list) or len(row) != 4:
            raise ValueError(f"{name} must be a 4x4 matrix")
        matrix.append([float(v) for v in row])
    return matrix


def camera_intrinsics(name: str, cam: Dict[str, Any]) -> Dict[str, Any]:
    camera_model = cam.get("camera_model")
    distortion_model = cam.get("distortion_model")
    if camera_model != "pinhole" or distortion_model != "equidistant":
        raise ValueError(
            f"{name}: unsupported Kalibr model {camera_model!r}/{distortion_model!r}; "
            "the current runtime converter only supports pinhole-equi -> kb4"
        )

    intrinsics = cam.get("intrinsics")
    distortion = cam.get("distortion_coeffs")
    if not isinstance(intrinsics, list) or len(intrinsics) != 4:
        raise ValueError(
            f"{name}: expected exactly four pinhole intrinsics [fx, fy, cx, cy]"
        )
    if not isinstance(distortion, list) or len(distortion) != 4:
        raise ValueError(
            f"{name}: expected exactly four equidistant distortion coefficients"
        )

    fx, fy, cx, cy = [float(v) for v in intrinsics]
    d = [float(v) for v in distortion]
    return {
        "camera_type": "kb4",
        "intrinsics": {
            "fx": fx,
            "fy": fy,
            "cx": cx,
            "cy": cy,
            "k1": d[0],
            "k2": d[1],
            "k3": d[2],
            "k4": d[3],
        },
    }


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--camchain", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--imu-update-rate", required=True, type=positive_float)
    ap.add_argument("--accel-noise-density", required=True, type=nonnegative_float)
    ap.add_argument("--accel-random-walk", required=True, type=nonnegative_float)
    ap.add_argument("--gyro-noise-density", required=True, type=nonnegative_float)
    ap.add_argument("--gyro-random-walk", required=True, type=nonnegative_float)
    ap.add_argument(
        "--no-imu",
        action="store_true",
        help=(
            "convert a stereo camera-only Kalibr camchain; use cam0 as the "
            "synthetic body frame and do not require T_cam_imu"
        ),
    )
    args = ap.parse_args()

    camchain_path = Path(args.camchain)
    out_path = Path(args.out)
    out_path.parent.mkdir(parents=True, exist_ok=True)

    loaded = yaml.safe_load(camchain_path.read_text(encoding="utf-8"))
    if not isinstance(loaded, dict):
        raise ValueError("camchain root must be a YAML mapping")
    try:
        cams = [loaded["cam0"], loaded["cam1"]]
    except KeyError as exc:
        raise ValueError(
            f"camchain is missing required camera entry: {exc.args[0]}"
        ) from exc
    if not all(isinstance(cam, dict) for cam in cams):
        raise ValueError("cam0 and cam1 must be YAML mappings")

    if args.no_imu:
        # A camera-only Kalibr camchain has no physical IMU frame.  Define the
        # runtime body frame as cam0.  Kalibr stores T_cn_cnm1 on cam1 as the
        # transform from cam0 to cam1.  XR Gate's T_imu_cam entries store the
        # inverse direction (camera -> body), so cam1 uses its inverse.
        identity = [
            [1.0, 0.0, 0.0, 0.0],
            [0.0, 1.0, 0.0, 0.0],
            [0.0, 0.0, 1.0, 0.0],
            [0.0, 0.0, 0.0, 1.0],
        ]
        T_cam1_cam0 = require_matrix4(
            "cam1.T_cn_cnm1", cams[1].get("T_cn_cnm1")
        )
        T_imu_cam = [
            pose_from_T(identity),
            pose_from_T(mat_inv(T_cam1_cam0)),
        ]
        cam_time_offset_ns = 0
    else:
        T_imu_cam = []
        for index, cam in enumerate(cams):
            # Kalibr gives T_cam_imu: imu -> camera.
            # The existing XR Gate Basalt JSON convention stores inverse(T_cam_imu).
            T_cam_imu = require_matrix4(
                f"cam{index}.T_cam_imu", cam.get("T_cam_imu")
            )
            T_imu_cam.append(pose_from_T(mat_inv(T_cam_imu)))

        shifts = [float(cam.get("timeshift_cam_imu", 0.0)) for cam in cams]
        if not all(math.isfinite(shift) for shift in shifts):
            raise ValueError("camera-to-IMU time shifts must be finite")
        cam_time_offset_ns = int(round((sum(shifts) / len(shifts)) * 1e9))

    resolution = []
    for index, cam in enumerate(cams):
        value = cam.get("resolution")
        if not isinstance(value, list) or len(value) != 2:
            raise ValueError(f"cam{index}.resolution must contain [width, height]")
        width, height = int(value[0]), int(value[1])
        if width <= 0 or height <= 0:
            raise ValueError(f"cam{index}.resolution must be positive")
        resolution.append([width, height])

    data = {
        "value0": {
            "T_imu_cam": T_imu_cam,
            "intrinsics": [
                camera_intrinsics(f"cam{i}", cam)
                for i, cam in enumerate(cams)
            ],
            "resolution": resolution,
            "calib_accel_bias": [0.0] * 9,
            "calib_gyro_bias": [0.0] * 12,
            "imu_update_rate": float(args.imu_update_rate),
            "accel_noise_std": [float(args.accel_noise_density)] * 3,
            "gyro_noise_std": [float(args.gyro_noise_density)] * 3,
            "accel_bias_std": [float(args.accel_random_walk)] * 3,
            "gyro_bias_std": [float(args.gyro_random_walk)] * 3,
            "T_mocap_world": {
                "px": 0.0,
                "py": 0.0,
                "pz": 0.0,
                "qx": 0.0,
                "qy": 0.0,
                "qz": 0.0,
                "qw": 1.0,
            },
            "T_imu_marker": {
                "px": 0.0,
                "py": 0.0,
                "pz": 0.0,
                "qx": 0.0,
                "qy": 0.0,
                "qz": 0.0,
                "qw": 1.0,
            },
            "mocap_time_offset_ns": 0,
            "mocap_to_imu_offset_ns": 0,
            "cam_time_offset_ns": cam_time_offset_ns,
            "vignette": [
                {
                    "value0": 0,
                    "value1": 10000000000,
                    "value2": [[1.0] for _ in range(67)],
                },
                {
                    "value0": 0,
                    "value1": 10000000000,
                    "value2": [[1.0] for _ in range(67)],
                },
            ],
        }
    }

    out_path.write_text(json.dumps(data, indent=4) + "\n", encoding="utf-8")

    print("written:", out_path)
    print("mode:", "stereo_vo_no_imu" if args.no_imu else "stereo_vio")
    print("resolution:", data["value0"]["resolution"])
    print("imu_update_rate:", data["value0"]["imu_update_rate"])
    print("cam_time_offset_ns:", cam_time_offset_ns)


if __name__ == "__main__":
    main()
