#!/usr/bin/env python3
"""Quick visual viewer for capture_service stereo frames over POSIX SHM.

Run from the xr_tracking repo root while capture_service is already running:

  python3 tools/view_capture_service_shm.py \
    --registry /tmp/capture_service_streams.json \
    --cam0 camera0 --cam1 camera1 --imu imu0

Keys:
  q / Esc  quit
  p        pause/resume
  s        save current stereo PNG into /tmp
  h        print help
"""

from __future__ import annotations

import argparse
import os
import sys
import time
from pathlib import Path
from typing import Optional, Tuple


def find_project_root() -> Path:
    here = Path(__file__).resolve()
    for parent in [here.parent, *here.parents]:
        if (parent / "capture_service").exists():
            return parent
    return Path.cwd()


PROJECT_ROOT = find_project_root()
CAPTURE_SERVICE_ROOT = PROJECT_ROOT / "capture_service"
if CAPTURE_SERVICE_ROOT.exists():
    sys.path.insert(0, str(CAPTURE_SERVICE_ROOT))


def load_capture_client_modules():
    try:
        from capture_client import BasaltStereoImuSynchronizer, CaptureClient  # type: ignore
    except Exception as exc:
        raise RuntimeError(
            "failed to import capture_client; run from xr_tracking root or set "
            "PYTHONPATH=$ROOT_PROJECT"
        ) from exc
    return CaptureClient, BasaltStereoImuSynchronizer


def load_cv_np():
    try:
        import cv2  # type: ignore
        import numpy as np  # type: ignore
    except Exception as exc:
        raise RuntimeError("viewer requires python3-opencv and numpy") from exc
    return cv2, np


def frame_to_gray(frame):
    cv2, np = load_cv_np()

    width = int(getattr(frame, "width", 0) or 0)
    height = int(getattr(frame, "height", 0) or 0)
    fmt = str(getattr(frame, "format_name", "") or "").upper()
    payload = getattr(frame, "data", b"") or b""

    if width <= 0 or height <= 0 or not payload:
        return None, f"empty w={width} h={height} fmt={fmt!r} bytes={len(payload)}"

    if fmt != "GRAY8":
        return None, f"unsupported fmt={fmt!r}; expected GRAY8"

    expected = width * height
    arr = np.frombuffer(payload, dtype=np.uint8)
    if arr.size < expected:
        return None, f"short frame w={width} h={height} bytes={arr.size} expected={expected}"
    if arr.size > expected:
        arr = arr[:expected]

    return arr.reshape((height, width)).copy(), ""


def image_stats(gray):
    cv2, np = load_cv_np()
    if gray is None:
        return {
            "mean": 0.0,
            "std": 0.0,
            "black": 1.0,
            "white": 0.0,
            "corners": 0,
            "lap": 0.0,
        }

    total = int(gray.size)
    corners = cv2.goodFeaturesToTrack(gray, maxCorners=800, qualityLevel=0.01, minDistance=7.0)
    lap = cv2.Laplacian(gray, cv2.CV_64F)
    return {
        "mean": float(gray.mean()),
        "std": float(gray.std()),
        "black": float((gray <= 8).sum()) / float(max(1, total)),
        "white": float((gray >= 248).sum()) / float(max(1, total)),
        "corners": 0 if corners is None else int(len(corners)),
        "lap": float(lap.std()),
    }


def format_stats(name: str, stats: dict, seq: Optional[int], ts: Optional[int], err: str = "") -> str:
    seq_s = "-" if seq is None else str(seq)
    ts_s = "-" if ts is None else str(ts)
    suffix = f" err={err}" if err else ""
    return (
        f"{name}: seq={seq_s} ts={ts_s} "
        f"mean={stats['mean']:.1f} std={stats['std']:.1f} "
        f"black={stats['black']:.2f} white={stats['white']:.2f} "
        f"corners={stats['corners']} lap={stats['lap']:.1f}{suffix}"
    )


def frame_meta(frame) -> Tuple[Optional[int], Optional[int]]:
    seq = getattr(frame, "seq", None)
    if seq is None:
        seq = getattr(frame, "sequence", None)
    if seq is None:
        seq = getattr(frame, "frame_id", None)

    ts = getattr(frame, "timestamp_ns", None)
    if ts is None:
        ts = getattr(frame, "capture_timestamp_ns", None)
    if ts is None:
        ts = getattr(frame, "host_timestamp_ns", None)

    try:
        seq = None if seq is None else int(seq)
    except Exception:
        seq = None
    try:
        ts = None if ts is None else int(ts)
    except Exception:
        ts = None
    return seq, ts


def make_display(cam0, cam1, stats0, stats1, args):
    cv2, np = load_cv_np()

    def normalize(gray):
        if gray is None:
            return np.zeros((args.height, args.width), dtype=np.uint8)
        out = gray
        if args.rotate:
            if args.rotate == "cw90":
                out = cv2.rotate(out, cv2.ROTATE_90_CLOCKWISE)
            elif args.rotate == "ccw90":
                out = cv2.rotate(out, cv2.ROTATE_90_COUNTERCLOCKWISE)
            elif args.rotate == "180":
                out = cv2.rotate(out, cv2.ROTATE_180)
        if args.width > 0 and args.height > 0:
            out = cv2.resize(out, (args.width, args.height), interpolation=cv2.INTER_AREA)
        return out

    left = normalize(cam0)
    right = normalize(cam1)

    # Make heights equal even if user skipped resize.
    if left.shape[0] != right.shape[0]:
        h = min(left.shape[0], right.shape[0])
        left = left[:h, :]
        right = right[:h, :]

    stereo = np.hstack([left, right])
    stereo_bgr = cv2.cvtColor(stereo, cv2.COLOR_GRAY2BGR)

    text0 = f"cam0 black={stats0['black']:.2f} mean={stats0['mean']:.1f} corners={stats0['corners']} lap={stats0['lap']:.1f}"
    text1 = f"cam1 black={stats1['black']:.2f} mean={stats1['mean']:.1f} corners={stats1['corners']} lap={stats1['lap']:.1f}"

    cv2.putText(stereo_bgr, text0, (10, 24), cv2.FONT_HERSHEY_SIMPLEX, 0.55, (255, 255, 255), 1, cv2.LINE_AA)
    cv2.putText(stereo_bgr, text1, (left.shape[1] + 10, 24), cv2.FONT_HERSHEY_SIMPLEX, 0.55, (255, 255, 255), 1, cv2.LINE_AA)
    cv2.line(stereo_bgr, (left.shape[1], 0), (left.shape[1], stereo_bgr.shape[0] - 1), (255, 255, 255), 1)
    return stereo_bgr


def print_help():
    print(
        "keys: q/Esc quit | p pause/resume | s save /tmp/capture_service_stereo_*.png | h help",
        flush=True,
    )


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="Quick viewer for capture_service stereo SHM frames.")
    ap.add_argument("--registry", default="/tmp/capture_service_streams.json")
    ap.add_argument("--cam0", default="camera0")
    ap.add_argument("--cam1", default="camera1")
    ap.add_argument("--imu", default="imu0")
    ap.add_argument("--stereo-max-delta-ms", type=float, default=1.0)
    ap.add_argument("--wait-for-imu-s", type=float, default=0.05)
    ap.add_argument("--read-timeout-s", type=float, default=1.0)
    ap.add_argument("--width", type=int, default=480, help="display width per camera; <=0 keeps original")
    ap.add_argument("--height", type=int, default=640, help="display height per camera; <=0 keeps original")
    ap.add_argument("--rotate", choices=["none", "cw90", "ccw90", "180"], default="none")
    ap.add_argument("--print-every", type=int, default=30)
    ap.add_argument("--warn-black", type=float, default=0.98)
    ap.add_argument("--save-dir", default="/tmp")
    args = ap.parse_args(argv)

    if args.rotate == "none":
        args.rotate = ""

    cv2, _np = load_cv_np()
    CaptureClient, BasaltStereoImuSynchronizer = load_capture_client_modules()

    client = None
    print_help()
    print(f"[viewer] registry={args.registry} cam0={args.cam0} cam1={args.cam1} imu={args.imu}", flush=True)

    try:
        client = CaptureClient.from_shm_registry(
            args.registry,
            required_streams=[args.cam0, args.cam1, args.imu],
        )
        sync = BasaltStereoImuSynchronizer(
            client,
            cam0_stream=args.cam0,
            cam1_stream=args.cam1,
            imu_stream=args.imu,
            stereo_max_delta_ns=int(args.stereo_max_delta_ms * 1_000_000.0),
            wait_for_imu_s=args.wait_for_imu_s,
        )

        paused = False
        shown = 0
        dropped = 0
        black0_count = 0
        black1_count = 0
        last_print = time.monotonic()
        fps_start = time.monotonic()
        last_image = None

        while True:
            if not paused:
                packet = sync.read_next(timeout_s=args.read_timeout_s, copy_images=True)
                if packet is None:
                    dropped += 1
                    print(f"[viewer] WARN sync timeout dropped={dropped}", flush=True)
                    key = cv2.waitKey(1) & 0xFF
                    if key in (27, ord("q")):
                        break
                    continue

                cam0, err0 = frame_to_gray(packet.pair.cam0)
                cam1, err1 = frame_to_gray(packet.pair.cam1)
                stats0 = image_stats(cam0)
                stats1 = image_stats(cam1)
                seq0, ts0 = frame_meta(packet.pair.cam0)
                seq1, ts1 = frame_meta(packet.pair.cam1)

                if stats0["black"] >= args.warn_black:
                    black0_count += 1
                if stats1["black"] >= args.warn_black:
                    black1_count += 1

                shown += 1
                last_image = make_display(cam0, cam1, stats0, stats1, args)
                cv2.imshow("capture_service SHM stereo: cam0 | cam1", last_image)

                now = time.monotonic()
                should_print = (
                    args.print_every > 0 and shown % args.print_every == 0
                ) or stats0["black"] >= args.warn_black or stats1["black"] >= args.warn_black

                if should_print:
                    dt = max(1e-6, now - fps_start)
                    fps = shown / dt
                    print(
                        f"[viewer] frames={shown} fps={fps:.2f} sync_timeouts={dropped} "
                        f"black0={black0_count} black1={black1_count}",
                        flush=True,
                    )
                    print("[viewer] " + format_stats("cam0", stats0, seq0, ts0, err0), flush=True)
                    print("[viewer] " + format_stats("cam1", stats1, seq1, ts1, err1), flush=True)
                    if getattr(packet, "imu_samples", None) is not None:
                        print(f"[viewer] imu_samples={len(packet.imu_samples)}", flush=True)
                    last_print = now

            key = cv2.waitKey(1 if not paused else 50) & 0xFF
            if key in (27, ord("q")):
                break
            if key == ord("p"):
                paused = not paused
                print(f"[viewer] paused={paused}", flush=True)
            elif key == ord("h"):
                print_help()
            elif key == ord("s"):
                if last_image is not None:
                    out = Path(args.save_dir) / f"capture_service_stereo_{int(time.time() * 1000)}.png"
                    cv2.imwrite(str(out), last_image)
                    print(f"[viewer] saved {out}", flush=True)

    except KeyboardInterrupt:
        print("[viewer] interrupted", flush=True)
        return 130
    except Exception as exc:
        print(f"[viewer][ERROR] {exc}", file=sys.stderr, flush=True)
        return 1
    finally:
        try:
            cv2.destroyAllWindows()
        except Exception:
            pass
        if client is not None:
            try:
                client.close()
            except Exception:
                pass

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
