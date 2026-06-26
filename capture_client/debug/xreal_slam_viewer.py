#!/usr/bin/env python3
from __future__ import annotations

import argparse
import os
import sys
import time
from pathlib import Path

import cv2
import numpy as np

def _prepend_sys_path(path: Path) -> None:
    text = str(path)
    if path.exists() and text not in sys.path:
        sys.path.insert(0, text)


def _setup_capture_client_import_path() -> None:
    # Source-tree layout:
    #   <repo>/capture_client/debug/xreal_slam_viewer.py
    # Packaged layout:
    #   out/xreal_ultra/bin/python/capture_client/debug/xreal_slam_viewer.py
    # In both cases Python needs the parent directory that contains the
    # capture_client package, not capture_client/ itself.
    candidates: list[Path] = []

    for env_name in ("CAPTURE_CLIENT_ROOT", "XR_PACKAGE_ROOT", "ROOT_PROJECT"):
        value = os.environ.get(env_name)
        if not value:
            continue
        root = Path(value).expanduser().resolve()
        candidates.extend([
            root,
            root / "bin/python",
            root / "capture_client",
        ])

    here = Path(__file__).resolve()
    for parent in [here.parent, *here.parents]:
        candidates.extend([
            parent,
            parent / "bin/python",
            parent / "capture_client",
        ])

    default_src = Path.home() / "src/xr_tracking"
    default_pkg = default_src / "out/xreal_ultra"
    candidates.extend([
        default_src,
        default_src / "capture_client",
        default_pkg / "bin/python",
        default_pkg / "bin/python/capture_client",
    ])

    for candidate in candidates:
        # Candidate may be the package dir itself or the parent containing it.
        if (candidate / "__init__.py").exists() and candidate.name == "capture_client":
            _prepend_sys_path(candidate.parent)
        elif (candidate / "capture_client" / "__init__.py").exists():
            _prepend_sys_path(candidate)


_setup_capture_client_import_path()

from capture_client import CaptureClient, StereoPairReader  # noqa: E402


def _make_client(args: argparse.Namespace) -> CaptureClient:
    required = [args.cam0_stream, args.cam1_stream]
    transport = args.transport.strip().lower()
    if transport == "shm":
        return CaptureClient.from_shm_registry(args.registry, required_streams=required)
    if transport in ("tcp", "capture_tcp"):
        return CaptureClient.from_tcp(
            args.tcp_host,
            args.tcp_port,
            required_streams=required,
            subscribe_streams=required,
        )
    raise ValueError(f"unsupported transport {args.transport!r}; expected shm or tcp")


def _gray8_to_np(frame) -> np.ndarray:
    if frame.format_name != "GRAY8":
        raise RuntimeError(f"{frame.stream_id}: expected GRAY8, got {frame.format_name}")
    expected = int(frame.width) * int(frame.height)
    if len(frame.data) < expected:
        raise RuntimeError(f"{frame.stream_id}: payload too small: {len(frame.data)} < {expected}")
    return np.frombuffer(frame.data, dtype=np.uint8, count=expected).reshape(
        (int(frame.height), int(frame.width))
    )


def _put_label(img: np.ndarray, text: str, y: int) -> None:
    cv2.putText(
        img,
        text,
        (10, y),
        cv2.FONT_HERSHEY_SIMPLEX,
        0.5,
        (255, 255, 255),
        1,
        cv2.LINE_AA,
    )


def main() -> int:
    ap = argparse.ArgumentParser(description="Debug viewer for XREAL stereo SLAM streams")
    ap.add_argument("--transport", choices=["shm", "tcp", "capture_tcp"], default="shm")
    ap.add_argument("--registry", default="/tmp/capture_service_streams.json")
    ap.add_argument("--tcp-host", default="127.0.0.1")
    ap.add_argument("--tcp-port", type=int, default=45660)
    ap.add_argument("--cam0-stream", default="camera0")
    ap.add_argument("--cam1-stream", default="camera1")
    ap.add_argument("--max-delta-ms", type=float, default=1.0)
    ap.add_argument("--window", default="XREAL SLAM stereo viewer")
    ap.add_argument("--print-every", type=int, default=30)
    ap.add_argument("--scale", type=float, default=1.0)
    args = ap.parse_args()

    client = _make_client(args)
    streams = client.list_streams()
    for sid in (args.cam0_stream, args.cam1_stream):
        info = streams[sid]
        print(
            f"{sid}: {info.width}x{info.height} fmt={info.format_name} "
            f"payload={info.payload_size} kind={info.kind}"
        )

    reader = StereoPairReader(
        client,
        args.cam0_stream,
        args.cam1_stream,
        max_timestamp_delta_ns=int(args.max_delta_ms * 1_000_000),
    )

    cv2.namedWindow(args.window, cv2.WINDOW_NORMAL)
    count = 0
    t0 = time.monotonic()
    last_print = t0

    try:
        while True:
            pair = reader.read_next_pair(timeout_s=1.0, copy_payload=True)
            if pair is None:
                print("waiting for stereo frames...")
                key = cv2.waitKey(20) & 0xFF
                if key in (27, ord("q")):
                    break
                continue

            cam0 = _gray8_to_np(pair.cam0)
            cam1 = _gray8_to_np(pair.cam1)
            if cam0.shape != cam1.shape:
                h = min(cam0.shape[0], cam1.shape[0])
                w = min(cam0.shape[1], cam1.shape[1])
                cam0 = cam0[:h, :w]
                cam1 = cam1[:h, :w]

            vis = np.concatenate([cam0, cam1], axis=1)
            vis = cv2.cvtColor(vis, cv2.COLOR_GRAY2BGR)

            count += 1
            now = time.monotonic()
            elapsed = max(1e-6, now - t0)
            fps = count / elapsed
            delta_ms = pair.timestamp_delta_ns / 1e6
            h, w = cam0.shape

            _put_label(vis, f"cam0 {pair.cam0.sequence} {w}x{h}", 22)
            _put_label(vis, f"cam1 {pair.cam1.sequence} delta={delta_ms:.3f} ms fps={fps:.2f}", 44)

            if args.scale != 1.0:
                vis = cv2.resize(vis, None, fx=args.scale, fy=args.scale, interpolation=cv2.INTER_NEAREST)

            cv2.imshow(args.window, vis)
            if args.print_every > 0 and count % args.print_every == 0:
                dt = max(1e-6, now - last_print)
                print(
                    f"frames={count} rate={args.print_every / dt:.2f}Hz "
                    f"size={w}x{h} seq=({pair.cam0.sequence},{pair.cam1.sequence}) "
                    f"delta_ms={delta_ms:.3f}"
                )
                last_print = now

            key = cv2.waitKey(1) & 0xFF
            if key in (27, ord("q")):
                break
    finally:
        client.close()
        cv2.destroyAllWindows()

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
