#!/usr/bin/env python3
from __future__ import annotations

import argparse
import csv
import time
from datetime import datetime
from pathlib import Path

import cv2
import numpy as np

from capture_client.client import CaptureClient
from capture_client.sync import BasaltStereoImuSynchronizer


def make_client(args):
    if args.transport == "tcp":
        return CaptureClient.from_tcp(
            args.tcp_host,
            args.tcp_port,
            required_streams=[args.cam0_stream, args.cam1_stream, args.imu_stream],
            subscribe_streams=[args.cam0_stream, args.cam1_stream, args.imu_stream],
        )
    return CaptureClient.from_shm_registry(
        args.registry,
        required_streams=[args.cam0_stream, args.cam1_stream, args.imu_stream],
    )


def image_to_numpy(frame):
    if frame.format_name != "GRAY8":
        raise RuntimeError(f"unsupported image format: {frame.format_name}")
    arr = np.frombuffer(frame.data, dtype=np.uint8)
    expected = frame.width * frame.height
    if arr.size != expected:
        raise RuntimeError(f"bad payload size: got {arr.size}, expected {expected}")
    return arr.reshape((frame.height, frame.width))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--transport", choices=["shm", "tcp"], default="shm")
    ap.add_argument("--registry", default="/tmp/capture_service_streams.json")
    ap.add_argument("--tcp-host", default="127.0.0.1")
    ap.add_argument("--tcp-port", type=int, default=45660)
    ap.add_argument("--cam0-stream", default="camera0")
    ap.add_argument("--cam1-stream", default="camera1")
    ap.add_argument("--imu-stream", default="imu0")
    ap.add_argument("--seconds", type=float, default=90.0)
    ap.add_argument("--out-root", default=str(Path.home() / "xreal_records"))
    ap.add_argument("--name", default="")
    ap.add_argument("--stereo-max-delta-ms", type=float, default=1.0)
    args = ap.parse_args()

    stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    name = args.name or f"xreal_unified480_calib_{stamp}"
    out = Path(args.out_root) / name
    cam0_dir = out / "cam0" / "data"
    cam1_dir = out / "cam1" / "data"
    cam0_dir.mkdir(parents=True, exist_ok=True)
    cam1_dir.mkdir(parents=True, exist_ok=True)

    client = make_client(args)
    sync = BasaltStereoImuSynchronizer(
        client,
        cam0_stream=args.cam0_stream,
        cam1_stream=args.cam1_stream,
        imu_stream=args.imu_stream,
        stereo_max_delta_ns=int(args.stereo_max_delta_ms * 1e6),
        wait_for_imu_s=0.05,
    )

    info = client.list_streams()
    with open(out / "streams.txt", "w") as f:
        for sid, s in info.items():
            f.write(f"{sid}: {s.width}x{s.height} {s.format_name} payload={s.payload_size}\\n")

    cam_csv = open(out / "camera_timestamps.csv", "w", newline="")
    imu_csv = open(out / "imu.csv", "w", newline="")
    cam_w = csv.writer(cam_csv)
    imu_w = csv.writer(imu_csv)

    cam_w.writerow(["timestamp_ns", "sequence", "cam0_file", "cam1_file", "cam0_ts_ns", "cam1_ts_ns", "stereo_delta_ns"])
    imu_w.writerow(["timestamp_ns", "gx", "gy", "gz", "ax", "ay", "az", "sequence"])

    start = time.monotonic()
    frames = 0
    imu_count = 0
    last_print = start

    try:
        while time.monotonic() - start < args.seconds:
            pkt = sync.read_next(timeout_s=1.0, copy_images=True)
            if pkt is None:
                continue

            ts = int(pkt.camera_timestamp_ns)
            seq = int(pkt.pair.sequence)
            cam0_name = f"{ts}.png"
            cam1_name = f"{ts}.png"

            img0 = image_to_numpy(pkt.pair.cam0)
            img1 = image_to_numpy(pkt.pair.cam1)

            cv2.imwrite(str(cam0_dir / cam0_name), img0)
            cv2.imwrite(str(cam1_dir / cam1_name), img1)

            cam_w.writerow([
                ts,
                seq,
                f"cam0/data/{cam0_name}",
                f"cam1/data/{cam1_name}",
                int(pkt.pair.cam0.timestamp_ns),
                int(pkt.pair.cam1.timestamp_ns),
                int(pkt.pair.timestamp_delta_ns),
            ])

            for s in pkt.imu_samples:
                gx, gy, gz = s.gyro_rad_s
                ax, ay, az = s.accel_m_s2
                imu_w.writerow([int(s.timestamp_ns), gx, gy, gz, ax, ay, az, int(s.sequence)])
                imu_count += 1

            frames += 1
            now = time.monotonic()
            if now - last_print >= 2.0:
                dt = now - start
                print(f"[record] frames={frames} imu={imu_count} fps={frames/dt:.2f} imu_rate={imu_count/dt:.1f} out={out}", flush=True)
                last_print = now
    finally:
        cam_csv.close()
        imu_csv.close()
        client.close()

    print(out)


if __name__ == "__main__":
    main()
