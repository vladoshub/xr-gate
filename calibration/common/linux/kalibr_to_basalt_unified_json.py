#!/usr/bin/env python3
import argparse
import json
import math
from pathlib import Path

import yaml


def mat_inv(T):
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


def mat_to_quat(R):
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

    n = math.sqrt(qx*qx + qy*qy + qz*qz + qw*qw)
    return qx / n, qy / n, qz / n, qw / n


def pose_from_T(T):
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


def camera_intrinsics(cam):
    fx, fy, cx, cy = cam["intrinsics"]
    d = list(cam["distortion_coeffs"])
    while len(d) < 4:
        d.append(0.0)

    return {
        "camera_type": "kb4",
        "intrinsics": {
            "fx": float(fx),
            "fy": float(fy),
            "cx": float(cx),
            "cy": float(cy),
            "k1": float(d[0]),
            "k2": float(d[1]),
            "k3": float(d[2]),
            "k4": float(d[3]),
        },
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--camchain", required=True)
    ap.add_argument("--out", required=True)
    args = ap.parse_args()

    camchain_path = Path(args.camchain)
    out_path = Path(args.out)
    out_path.parent.mkdir(parents=True, exist_ok=True)

    k = yaml.safe_load(camchain_path.read_text())
    cams = [k["cam0"], k["cam1"]]

    T_imu_cam = []
    for cam in cams:
        # Kalibr gives T_cam_imu: imu -> camera.
        # Basalt JSON field name T_imu_cam historically stores camera pose as T_imu_cam object.
        # In our previous working Basalt profile we used inverse(T_cam_imu).
        T_cam_imu = cam["T_cam_imu"]
        T_imu_cam.append(pose_from_T(mat_inv(T_cam_imu)))

    shifts = [float(cam.get("timeshift_cam_imu", 0.0)) for cam in cams]
    cam_time_offset_ns = int(round((sum(shifts) / len(shifts)) * 1e9))

    data = {
        "value0": {
            "T_imu_cam": T_imu_cam,
            "intrinsics": [camera_intrinsics(c) for c in cams],
            "resolution": [
                [int(c["resolution"][0]), int(c["resolution"][1])]
                for c in cams
            ],
            "calib_accel_bias": [0.0] * 9,
            "calib_gyro_bias": [0.0] * 12,
            "imu_update_rate": 1000.0,
            "accel_noise_std": [0.01, 0.01, 0.01],
            "gyro_noise_std": [0.001, 0.001, 0.001],
            "accel_bias_std": [0.001, 0.001, 0.001],
            "gyro_bias_std": [0.0001, 0.0001, 0.0001],
            "T_mocap_world": {
                "px": 0.0, "py": 0.0, "pz": 0.0,
                "qx": 0.0, "qy": 0.0, "qz": 0.0, "qw": 1.0,
            },
            "T_imu_marker": {
                "px": 0.0, "py": 0.0, "pz": 0.0,
                "qx": 0.0, "qy": 0.0, "qz": 0.0, "qw": 1.0,
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

    out_path.write_text(json.dumps(data, indent=4) + "\n")

    print("written:", out_path)
    print("resolution:", data["value0"]["resolution"])
    print("cam_time_offset_ns:", cam_time_offset_ns)


if __name__ == "__main__":
    main()
