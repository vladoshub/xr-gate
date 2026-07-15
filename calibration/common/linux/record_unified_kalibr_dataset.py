#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
from __future__ import annotations

import argparse
import csv
import json
import os
import platform
import signal
import subprocess
import sys
import time
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path
from typing import Optional

import cv2
import numpy as np


def _prepend_sys_path(path: Path) -> None:
    text = str(path)
    if path.exists() and text not in sys.path:
        sys.path.insert(0, text)


def _setup_capture_client_import_path() -> None:
    candidates = []
    for env_name in ("XR_PACKAGE_ROOT", "ROOT_PROJECT"):
        value = os.environ.get(env_name)
        if value:
            root = Path(value).expanduser().resolve()
            candidates.extend([
                root / "bin/python/capture_service",
                root / "bin/python",
                root / "capture_service",
                root,
            ])

    for base in [Path.cwd().resolve(), Path(__file__).resolve()]:
        for parent in [base, *base.parents]:
            candidates.extend([
                parent / "bin/python/capture_service",
                parent / "bin/python",
                parent / "capture_service",
                parent,
            ])

    default_pkg = Path.home() / "src/xr_tracking/out/xreal_ultra"
    default_src = Path.home() / "src/xr_tracking"
    candidates.extend([
        default_pkg / "bin/python/capture_service",
        default_pkg / "bin/python",
        default_src / "capture_service",
        default_src,
    ])

    for candidate in candidates:
        if (candidate / "capture_client").exists():
            _prepend_sys_path(candidate)


_setup_capture_client_import_path()

from capture_client.client import CaptureClient
from capture_client.sync import BasaltStereoImuSynchronizer


DEFAULT_REGISTRY = "/tmp/capture_service_streams.json"
DEFAULT_TCP_HOST = "127.0.0.1"
DEFAULT_TCP_PORT = 45660


def expand_path(value: str) -> Path:
    return Path(value).expanduser().resolve()


def resolve_transport(value: str) -> str:
    transport = value.strip().lower()
    if transport == "auto":
        return "tcp" if os.name == "nt" else "shm"
    return transport


def required_streams(args: argparse.Namespace) -> list[str]:
    streams = [args.cam0_stream, args.cam1_stream]
    if not args.no_imu:
        streams.append(args.imu_stream)
    return streams


def make_client(args: argparse.Namespace) -> CaptureClient:
    transport = resolve_transport(args.transport)
    required = required_streams(args)
    if transport == "tcp":
        return CaptureClient.from_tcp(
            args.tcp_host,
            args.tcp_port,
            required_streams=required,
            subscribe_streams=required,
        )
    if transport == "shm":
        return CaptureClient.from_shm_registry(args.registry, required_streams=required)
    raise ValueError(f"unsupported transport {args.transport!r}; expected auto, shm, or tcp")


def image_to_numpy(frame) -> np.ndarray:
    if frame.format_name != "GRAY8":
        raise RuntimeError(f"{frame.stream_id}: unsupported image format: {frame.format_name}")
    expected = int(frame.width) * int(frame.height)
    arr = np.frombuffer(frame.data, dtype=np.uint8, count=expected)
    if arr.size != expected:
        raise RuntimeError(f"{frame.stream_id}: bad payload size: got {arr.size}, expected {expected}")
    return arr.reshape((int(frame.height), int(frame.width)))


def check_streams(client: CaptureClient, args: argparse.Namespace, out: Path) -> dict:
    info = client.list_streams()
    missing = [sid for sid in required_streams(args) if sid not in info]
    if missing:
        raise RuntimeError(f"missing required streams: {missing}; available={sorted(info.keys())}")

    with open(out / "streams.txt", "w", encoding="utf-8") as f:
        for sid, stream in sorted(info.items()):
            f.write(
                f"{sid}: {stream.width}x{stream.height} {stream.format_name} "
                f"payload={stream.payload_size} kind={stream.kind}\n"
            )

    for sid in (args.cam0_stream, args.cam1_stream):
        stream = info[sid]
        if stream.format_name != "GRAY8":
            raise RuntimeError(f"{sid}: expected GRAY8, got {stream.format_name}")
        if not args.allow_size_mismatch:
            if int(stream.width) != args.expect_width or int(stream.height) != args.expect_height:
                raise RuntimeError(
                    f"{sid}: expected {args.expect_width}x{args.expect_height}, "
                    f"got {stream.width}x{stream.height}. Use --allow-size-mismatch to record anyway."
                )
    return info


def _tail_text(path: Path, *, max_lines: int = 80) -> str:
    try:
        lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
    except Exception as exc:
        return f"<cannot read {path}: {exc}>"
    if not lines:
        return f"<empty log: {path}>"
    return "\n".join(lines[-max_lines:])


def _remove_stale_registry_before_start(args: argparse.Namespace) -> None:
    if resolve_transport(args.transport) != "shm":
        return
    registry = expand_path(args.registry)
    try:
        if registry.exists():
            registry.unlink()
            print(f"[record] removed stale SHM registry before start: {registry}", flush=True)
    except Exception as exc:
        print(f"[record][WARN] could not remove stale SHM registry {registry}: {exc}", flush=True)


def _safe_latest_sequence(client: CaptureClient, stream_id: str) -> Optional[int]:
    try:
        return int(client.latest_sequence(stream_id))
    except BaseException:
        return None


def _safe_latest_image_summary(client: CaptureClient, stream_id: str) -> str:
    try:
        frame = client.read_latest_image(stream_id, copy_payload=False)
        if frame is None:
            return "latest=None"
        return (
            f"latest=seq{int(frame.sequence)} ts={int(frame.timestamp_ns)} "
            f"{int(frame.width)}x{int(frame.height)} {frame.format_name}"
        )
    except BaseException as exc:
        return f"latest_error={type(exc).__name__}: {exc}"


def _stream_debug_line(client: CaptureClient, args: argparse.Namespace) -> str:
    parts = []
    for sid in required_streams(args):
        parts.append(f"{sid}_latest_seq={_safe_latest_sequence(client, sid)}")
        if sid in (args.cam0_stream, args.cam1_stream):
            parts.append(f"{sid}_{_safe_latest_image_summary(client, sid)}")
    return "; ".join(parts)


def wait_for_stream_activity(client: CaptureClient, args: argparse.Namespace, *, timeout_s: float = 5.0) -> None:
    starts = {sid: _safe_latest_sequence(client, sid) for sid in required_streams(args)}
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        now = {sid: _safe_latest_sequence(client, sid) for sid in required_streams(args)}
        cameras_active = all(
            starts[sid] is not None and now[sid] is not None and now[sid] > starts[sid]
            for sid in (args.cam0_stream, args.cam1_stream)
        )
        imu_active = args.no_imu or (
            starts[args.imu_stream] is not None
            and now[args.imu_stream] is not None
            and now[args.imu_stream] > starts[args.imu_stream]
        )
        if cameras_active and imu_active:
            transitions = ", ".join(f"{sid} {starts[sid]}->{now[sid]}" for sid in required_streams(args))
            print(f"[record] stream activity OK: {transitions}", flush=True)
            return
        time.sleep(0.05)
    print(f"[record][WARN] stream activity did not advance cleanly: {_stream_debug_line(client, args)}", flush=True)


def default_package_root() -> Path:
    env = os.environ.get("XR_PACKAGE_ROOT") or os.environ.get("ROOT_PROJECT")
    if env:
        root = expand_path(env)
        if (root / "bin").exists():
            return root
    candidate = Path.home() / "src/xr_tracking/out/xreal_ultra"
    return candidate.resolve()


def start_capture_service_if_requested(args: argparse.Namespace) -> Optional[subprocess.Popen]:
    if not args.start_capture_service:
        return None
    if os.name == "nt":
        raise RuntimeError("--start-capture-service is currently Linux-only; start the Windows service separately")

    script = expand_path(args.capture_start_script)
    if not script.exists():
        raise RuntimeError(f"capture_service start script not found: {script}")

    _remove_stale_registry_before_start(args)
    log_path = expand_path(args.capture_service_log)
    log_path.parent.mkdir(parents=True, exist_ok=True)

    command = [str(script)]
    if args.capture_config:
        command.extend(["--config", str(expand_path(args.capture_config))])
    command.extend([
        "--publish", args.publish,
        "--registry", args.registry,
        "--tcp-bind", args.tcp_bind_host,
        "--tcp-port", str(args.tcp_port),
    ])
    command.extend(args.capture_extra_arg)

    env = os.environ.copy()
    env.setdefault("STOP_EXISTING", "1")
    env.setdefault("CLEAN_SHM", "1")
    env.setdefault("PYTHONUNBUFFERED", "1")

    print(f"[record] starting capture_service: {' '.join(command)}", flush=True)
    print(f"[record] capture_service log: {log_path}", flush=True)
    log_file = open(log_path, "w", encoding="utf-8")
    proc = subprocess.Popen(
        command,
        cwd=str(expand_path(args.package_root)) if args.package_root else None,
        env=env,
        stdout=log_file,
        stderr=subprocess.STDOUT,
        text=True,
        start_new_session=True,
    )
    proc._xr_log_file = log_file  # type: ignore[attr-defined]
    proc._xr_log_path = log_path  # type: ignore[attr-defined]
    return proc


def stop_capture_service(proc: Optional[subprocess.Popen]) -> None:
    if proc is None:
        return
    if proc.poll() is None:
        print("[record] stopping capture_service", flush=True)
        try:
            os.killpg(proc.pid, signal.SIGTERM)
        except Exception:
            proc.terminate()
        try:
            proc.wait(timeout=5.0)
        except subprocess.TimeoutExpired:
            try:
                os.killpg(proc.pid, signal.SIGKILL)
            except Exception:
                proc.kill()
            proc.wait(timeout=5.0)
    log_file = getattr(proc, "_xr_log_file", None)
    if log_file:
        log_file.close()


def wait_for_client(args: argparse.Namespace, capture_proc: Optional[subprocess.Popen]) -> CaptureClient:
    deadline = time.monotonic() + args.wait_streams_timeout_s
    last_error: Optional[BaseException] = None
    while time.monotonic() < deadline:
        if capture_proc is not None and capture_proc.poll() is not None:
            log_path = getattr(capture_proc, "_xr_log_path", None)
            tail = _tail_text(log_path) if isinstance(log_path, Path) else "<no capture_service log>"
            raise RuntimeError(
                f"capture_service exited before streams became available, code={capture_proc.returncode}.\n{tail}"
            )
        try:
            return make_client(args)
        except BaseException as exc:
            last_error = exc
            time.sleep(0.25)
    raise RuntimeError(f"timed out waiting for capture streams after {args.wait_streams_timeout_s}s: {last_error}")


@dataclass
class CameraOnlyPair:
    cam0: object
    cam1: object
    sequence: int
    timestamp_ns: int
    timestamp_delta_ns: int


def read_camera_only_pair(
    client: CaptureClient,
    args: argparse.Namespace,
    last_sequence: int,
    timeout_s: float,
) -> Optional[CameraOnlyPair]:
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        seq0 = _safe_latest_sequence(client, args.cam0_stream)
        seq1 = _safe_latest_sequence(client, args.cam1_stream)
        if seq0 is None or seq1 is None:
            time.sleep(0.002)
            continue
        sequence = min(seq0, seq1)
        if sequence <= last_sequence:
            time.sleep(0.002)
            continue

        cam0 = client.read_image_sequence(args.cam0_stream, sequence, copy_payload=True)
        cam1 = client.read_image_sequence(args.cam1_stream, sequence, copy_payload=True)
        if cam0 is None or cam1 is None:
            time.sleep(0.002)
            continue

        delta = abs(int(cam0.timestamp_ns) - int(cam1.timestamp_ns))
        if delta > int(args.stereo_max_delta_ms * 1e6):
            print(
                f"[record][WARN] dropping stereo sequence {sequence}: delta={delta}ns exceeds limit",
                flush=True,
            )
            last_sequence = sequence
            continue

        return CameraOnlyPair(
            cam0=cam0,
            cam1=cam1,
            sequence=sequence,
            timestamp_ns=max(int(cam0.timestamp_ns), int(cam1.timestamp_ns)),
            timestamp_delta_ns=delta,
        )
    return None


def write_metadata(out: Path, args: argparse.Namespace, info: dict, frames: int, imu_count: int) -> None:
    data = {
        "created_at": datetime.now().isoformat(timespec="seconds"),
        "target": args.target_name,
        "host": platform.node(),
        "platform": platform.platform(),
        "script": Path(__file__).name,
        "record_mode": "camera_only" if args.no_imu else "stereo_imu",
        "transport": resolve_transport(args.transport),
        "registry": args.registry,
        "tcp_host": args.tcp_host,
        "tcp_port": args.tcp_port,
        "capture_service": "started" if args.start_capture_service else "external",
        "capture_config": args.capture_config,
        "streams": {
            sid: {
                "width": int(stream.width),
                "height": int(stream.height),
                "format_name": stream.format_name,
                "payload_size": int(stream.payload_size),
                "kind": stream.kind,
            }
            for sid, stream in sorted(info.items())
        },
        "recorded_frames": frames,
        "recorded_imu_samples": imu_count,
        "stereo_max_delta_ms": args.stereo_max_delta_ms,
        "duration_requested_s": args.seconds,
    }
    (out / "record_metadata.json").write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")


def main() -> int:
    ap = argparse.ArgumentParser(description="Record a generic XR stereo or stereo+IMU Kalibr dataset")
    ap.add_argument("--target-name", default="generic")
    ap.add_argument("--transport", choices=["auto", "shm", "tcp"], default="auto")
    ap.add_argument("--registry", default=DEFAULT_REGISTRY)
    ap.add_argument("--tcp-host", default=DEFAULT_TCP_HOST)
    ap.add_argument("--tcp-port", type=int, default=DEFAULT_TCP_PORT)
    ap.add_argument("--tcp-bind-host", default="127.0.0.1")
    ap.add_argument("--cam0-stream", default="camera0")
    ap.add_argument("--cam1-stream", default="camera1")
    ap.add_argument("--imu-stream", default="imu0")
    ap.add_argument("--no-imu", action="store_true", help="record stereo only; useful before the external IMU is available")
    ap.add_argument("--seconds", type=float, default=90.0)
    ap.add_argument("--out-root", default=str(Path.home() / "xr_records"))
    ap.add_argument("--name", default="")
    ap.add_argument("--name-prefix", default="xr_calib")
    ap.add_argument("--stereo-max-delta-ms", type=float, default=1.0)
    ap.add_argument("--expect-width", type=int, default=640)
    ap.add_argument("--expect-height", type=int, default=480)
    ap.add_argument("--allow-size-mismatch", action="store_true")
    ap.add_argument("--warmup-seconds", type=float, default=2.0)
    ap.add_argument("--wait-streams-timeout-s", type=float, default=20.0)
    ap.add_argument("--sync-debug-every-s", type=float, default=2.0)

    ap.add_argument("--start-capture-service", action="store_true")
    ap.add_argument("--stop-capture-service", action="store_true")
    ap.add_argument("--package-root", default="")
    ap.add_argument("--capture-start-script", default="")
    ap.add_argument("--capture-config", default="")
    ap.add_argument("--capture-extra-arg", action="append", default=[])
    ap.add_argument("--publish", default="shm")
    ap.add_argument("--capture-service-log", default="/tmp/xr_calibration_capture_service.log")
    args = ap.parse_args()

    if args.start_capture_service and not args.capture_start_script:
        package_root = expand_path(args.package_root) if args.package_root else default_package_root()
        args.capture_start_script = str(package_root / "bin/scripts/capture_service_cpp/start_capture_service_cpp.sh")
        args.package_root = str(package_root)

    resolved_transport = resolve_transport(args.transport)
    if args.start_capture_service and resolved_transport == "tcp" and args.publish == "shm":
        args.publish = "tcp"
    if args.start_capture_service and resolved_transport == "shm" and "shm" not in args.publish.split(","):
        args.publish = "shm"

    stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    name = args.name or f"{args.name_prefix}_{stamp}"
    out = expand_path(args.out_root) / name
    cam0_dir = out / "cam0/data"
    cam1_dir = out / "cam1/data"
    cam0_dir.mkdir(parents=True, exist_ok=True)
    cam1_dir.mkdir(parents=True, exist_ok=True)

    capture_proc: Optional[subprocess.Popen] = None
    client: Optional[CaptureClient] = None
    frames = 0
    imu_count = 0
    info = {}

    try:
        capture_proc = start_capture_service_if_requested(args)
        client = wait_for_client(args, capture_proc)
        info = check_streams(client, args, out)

        if args.warmup_seconds > 0:
            print(f"[record] warmup {args.warmup_seconds:.1f}s", flush=True)
            time.sleep(args.warmup_seconds)
        wait_for_stream_activity(client, args)

        synchronizer = None
        if not args.no_imu:
            synchronizer = BasaltStereoImuSynchronizer(
                client,
                cam0_stream=args.cam0_stream,
                cam1_stream=args.cam1_stream,
                imu_stream=args.imu_stream,
                stereo_max_delta_ns=int(args.stereo_max_delta_ms * 1e6),
                wait_for_imu_s=0.05,
            )

        with (
            open(out / "camera_timestamps.csv", "w", newline="", encoding="utf-8") as cam_csv,
            open(out / "imu.csv", "w", newline="", encoding="utf-8") as imu_csv,
            open(out / "frame_quality.csv", "w", newline="", encoding="utf-8") as quality_csv,
        ):
            cam_w = csv.writer(cam_csv)
            imu_w = csv.writer(imu_csv)
            quality_w = csv.writer(quality_csv)
            cam_w.writerow(["timestamp_ns", "sequence", "cam0_file", "cam1_file", "cam0_ts_ns", "cam1_ts_ns", "stereo_delta_ns"])
            imu_w.writerow(["timestamp_ns", "gx", "gy", "gz", "ax", "ay", "az", "sequence"])
            quality_w.writerow(["timestamp_ns", "sequence", "cam0_mean", "cam0_std", "cam1_mean", "cam1_std"])

            start = time.monotonic()
            last_print = start
            last_no_packet_print = start
            last_camera_sequence = -1
            no_packet_count = 0
            print(f"[record] writing dataset: {out}", flush=True)
            print(
                f"[record] target={args.target_name} mode={'camera_only' if args.no_imu else 'stereo_imu'} "
                f"transport={resolved_transport}",
                flush=True,
            )

            while time.monotonic() - start < args.seconds:
                imu_samples = []
                if args.no_imu:
                    pair = read_camera_only_pair(client, args, last_camera_sequence, timeout_s=1.0)
                    if pair is None:
                        packet = None
                    else:
                        packet = pair
                        last_camera_sequence = pair.sequence
                else:
                    packet = synchronizer.read_next(timeout_s=1.0, copy_images=True)  # type: ignore[union-attr]
                    if packet is not None:
                        imu_samples = packet.imu_samples
                        pair = packet.pair
                    else:
                        pair = None

                if packet is None or pair is None:
                    no_packet_count += 1
                    now = time.monotonic()
                    if args.sync_debug_every_s > 0 and now - last_no_packet_print >= args.sync_debug_every_s:
                        print(f"[record][WARN] no stereo packet after {no_packet_count} waits: {_stream_debug_line(client, args)}", flush=True)
                        last_no_packet_print = now
                    continue

                if args.no_imu:
                    ts = int(pair.timestamp_ns)
                    seq = int(pair.sequence)
                    cam0_frame = pair.cam0
                    cam1_frame = pair.cam1
                    stereo_delta_ns = int(pair.timestamp_delta_ns)
                else:
                    ts = int(packet.camera_timestamp_ns)
                    seq = int(pair.sequence)
                    cam0_frame = pair.cam0
                    cam1_frame = pair.cam1
                    stereo_delta_ns = int(pair.timestamp_delta_ns)

                cam0_name = f"{ts}.png"
                cam1_name = f"{ts}.png"
                img0 = image_to_numpy(cam0_frame)
                img1 = image_to_numpy(cam1_frame)
                if not cv2.imwrite(str(cam0_dir / cam0_name), img0):
                    raise RuntimeError(f"failed to write {cam0_dir / cam0_name}")
                if not cv2.imwrite(str(cam1_dir / cam1_name), img1):
                    raise RuntimeError(f"failed to write {cam1_dir / cam1_name}")

                cam_w.writerow([
                    ts,
                    seq,
                    f"cam0/data/{cam0_name}",
                    f"cam1/data/{cam1_name}",
                    int(cam0_frame.timestamp_ns),
                    int(cam1_frame.timestamp_ns),
                    stereo_delta_ns,
                ])
                quality_w.writerow([ts, seq, float(img0.mean()), float(img0.std()), float(img1.mean()), float(img1.std())])

                for sample in imu_samples:
                    gx, gy, gz = sample.gyro_rad_s
                    ax, ay, az = sample.accel_m_s2
                    imu_w.writerow([int(sample.timestamp_ns), gx, gy, gz, ax, ay, az, int(sample.sequence)])
                    imu_count += 1

                frames += 1
                now = time.monotonic()
                if now - last_print >= 2.0:
                    elapsed = now - start
                    print(
                        f"[record] frames={frames} imu={imu_count} fps={frames/elapsed:.2f} "
                        f"imu_rate={imu_count/elapsed:.1f} out={out}",
                        flush=True,
                    )
                    last_print = now

        if frames == 0:
            raise RuntimeError("recorded zero frames; inspect stream diagnostics and the capture-service log")
    finally:
        if client:
            client.close()
        if info:
            write_metadata(out, args, info, frames, imu_count)
        if args.stop_capture_service:
            stop_capture_service(capture_proc)

    print(out)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
