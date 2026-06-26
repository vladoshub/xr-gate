#!/usr/bin/env python3
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
    # Package mode keeps runtime Python modules under:
    #   out/xreal_ultra/bin/python/capture_client
    # Source-tree mode keeps them under:
    #   capture_client
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
REQUIRED_STREAMS = ("camera0", "camera1", "imu0")


def expand_path(value: str) -> Path:
    return Path(value).expanduser().resolve()


def resolve_transport(value: str) -> str:
    transport = value.strip().lower()
    if transport == "auto":
        # Windows package mode is TCP-only for capture_service_cpp.
        return "tcp" if os.name == "nt" else "shm"
    return transport


def make_client(args) -> CaptureClient:
    transport = resolve_transport(args.transport)
    required = [args.cam0_stream, args.cam1_stream, args.imu_stream]
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
    required = [args.cam0_stream, args.cam1_stream, args.imu_stream]
    missing = [sid for sid in required if sid not in info]
    if missing:
        raise RuntimeError(f"missing required streams: {missing}; available={sorted(info.keys())}")

    with open(out / "streams.txt", "w", encoding="utf-8") as f:
        for sid, s in sorted(info.items()):
            f.write(f"{sid}: {s.width}x{s.height} {s.format_name} payload={s.payload_size} kind={s.kind}\n")

    for sid in (args.cam0_stream, args.cam1_stream):
        s = info[sid]
        if s.format_name != "GRAY8":
            raise RuntimeError(f"{sid}: expected GRAY8, got {s.format_name}")
        if not args.allow_size_mismatch:
            if int(s.width) != args.expect_width or int(s.height) != args.expect_height:
                raise RuntimeError(
                    f"{sid}: expected {args.expect_width}x{args.expect_height}, "
                    f"got {s.width}x{s.height}. Use --allow-size-mismatch to record anyway."
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
    # When the recorder starts capture_service itself, an old registry can still
    # exist from xreal_slam_viewer/debug runs. If wait_for_client() attaches to
    # that stale registry before the new service has cleaned/recreated it, the
    # recorder can see a frozen latest_seq forever and write zero frames.
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
        return f"latest=seq{int(frame.sequence)} ts={int(frame.timestamp_ns)} {int(frame.width)}x{int(frame.height)} {frame.format_name}"
    except BaseException as exc:
        return f"latest_error={type(exc).__name__}: {exc}"


def _safe_sequence_image_summary(client: CaptureClient, stream_id: str, sequence: Optional[int]) -> str:
    if not sequence or sequence <= 0:
        return "seq_read=skip"
    try:
        frame = client.read_image_sequence(stream_id, sequence, copy_payload=False)
        if frame is None:
            return f"seq_read[{sequence}]=None"
        return f"seq_read[{sequence}]=ok ts={int(frame.timestamp_ns)}"
    except BaseException as exc:
        return f"seq_read[{sequence}]_error={type(exc).__name__}: {exc}"


def _stream_debug_line(client: CaptureClient, args: argparse.Namespace) -> str:
    cam0_seq = _safe_latest_sequence(client, args.cam0_stream)
    cam1_seq = _safe_latest_sequence(client, args.cam1_stream)
    imu_seq = _safe_latest_sequence(client, args.imu_stream)
    target = min(x for x in (cam0_seq, cam1_seq) if x is not None) if cam0_seq is not None and cam1_seq is not None else None
    parts = [
        f"{args.cam0_stream}_latest_seq={cam0_seq}",
        f"{args.cam1_stream}_latest_seq={cam1_seq}",
        f"{args.imu_stream}_latest_seq={imu_seq}",
        f"{args.cam0_stream}_{_safe_latest_image_summary(client, args.cam0_stream)}",
        f"{args.cam1_stream}_{_safe_latest_image_summary(client, args.cam1_stream)}",
        f"{args.cam0_stream}_{_safe_sequence_image_summary(client, args.cam0_stream, target)}",
        f"{args.cam1_stream}_{_safe_sequence_image_summary(client, args.cam1_stream, target)}",
    ]
    return "; ".join(parts)


def wait_for_stream_activity(client: CaptureClient, args: argparse.Namespace, *, timeout_s: float = 5.0) -> None:
    start0 = _safe_latest_sequence(client, args.cam0_stream)
    start1 = _safe_latest_sequence(client, args.cam1_stream)
    starti = _safe_latest_sequence(client, args.imu_stream)
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        now0 = _safe_latest_sequence(client, args.cam0_stream)
        now1 = _safe_latest_sequence(client, args.cam1_stream)
        nowi = _safe_latest_sequence(client, args.imu_stream)
        cam0_ok = start0 is not None and now0 is not None and now0 > start0
        cam1_ok = start1 is not None and now1 is not None and now1 > start1
        # IMU can be absent/broken in diagnostics, but camera recording still needs to fail loudly later.
        imu_ok = starti is not None and nowi is not None and nowi > starti
        if cam0_ok and cam1_ok:
            print(
                f"[record] stream activity OK: {args.cam0_stream} {start0}->{now0}, "
                f"{args.cam1_stream} {start1}->{now1}, {args.imu_stream} {starti}->{nowi} "
                f"imu_active={imu_ok}",
                flush=True,
            )
            return
        time.sleep(0.05)
    print(f"[record][WARN] stream activity did not advance cleanly: {_stream_debug_line(client, args)}", flush=True)


def default_package_root() -> Path:
    env = os.environ.get("XR_PACKAGE_ROOT") or os.environ.get("ROOT_PROJECT")
    if env:
        return expand_path(env)

    cwd = Path.cwd().resolve()
    if (cwd / "devices/xreal_ultra").exists() and (cwd / "bin").exists():
        return cwd

    candidate = Path.home() / "src/xr_tracking/out/xreal_ultra"
    if candidate.exists():
        return candidate.resolve()

    return Path.home() / "src/xr_tracking/out/xreal_ultra"


def start_capture_service_if_requested(args: argparse.Namespace) -> Optional[subprocess.Popen]:
    if not args.start_capture_service:
        return None
    if os.name == "nt":
        raise RuntimeError("--start-capture-service is currently Linux-package only; start Windows TCP service separately")

    package_root = expand_path(args.package_root) if args.package_root else default_package_root()
    script = package_root / "devices/xreal_ultra/linux/scripts/capture_service/start_capture_service.sh"
    if not script.exists():
        raise RuntimeError(f"capture_service start script not found: {script}")

    _remove_stale_registry_before_start(args)

    log_path = expand_path(args.capture_service_log)
    log_path.parent.mkdir(parents=True, exist_ok=True)

    env = os.environ.copy()
    env["CAPTURE_SERVICE_IMPL"] = args.capture_service_impl
    env["PUBLISH"] = args.publish
    env["REGISTRY_PATH"] = args.registry
    env["TCP_PORT"] = str(args.tcp_port)
    env["TCP_BIND_HOST"] = args.tcp_bind_host
    env.setdefault("STOP_EXISTING", "1")
    env.setdefault("CLEAN_SHM", "1")
    env.setdefault("PYTHONUNBUFFERED", "1")

    # Optional orientation overrides for capture_service_cpp calibration/debug runs.
    for src_name, env_name in (
        ("left_rotate", "XR_CAPTURE_CPP_LEFT_ROTATE"),
        ("right_rotate", "XR_CAPTURE_CPP_RIGHT_ROTATE"),
        ("left_flip", "XR_CAPTURE_CPP_LEFT_FLIP"),
        ("right_flip", "XR_CAPTURE_CPP_RIGHT_FLIP"),
    ):
        value = getattr(args, src_name)
        if value:
            env[env_name] = value

    print(f"[record] starting capture_service: impl={args.capture_service_impl} publish={args.publish}", flush=True)
    print(f"[record] start script: {script}", flush=True)
    print(f"[record] capture_service log: {log_path}", flush=True)
    log_file = open(log_path, "w", encoding="utf-8")
    proc = subprocess.Popen(
        [str(script)],
        cwd=str(package_root),
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
    if proc.poll() is not None:
        log_file = getattr(proc, "_xr_log_file", None)
        if log_file:
            log_file.close()
        return
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


def wait_for_client(args: argparse.Namespace, capture_proc: Optional[subprocess.Popen] = None) -> CaptureClient:
    deadline = time.monotonic() + args.wait_streams_timeout_s
    last_error: Optional[BaseException] = None
    while time.monotonic() < deadline:
        if capture_proc is not None and capture_proc.poll() is not None:
            log_path = getattr(capture_proc, "_xr_log_path", None)
            tail = _tail_text(log_path) if isinstance(log_path, Path) else "<no capture_service log path>"
            raise RuntimeError(
                f"capture_service exited before streams became available, code={capture_proc.returncode}. "
                f"Last capture_service log lines:\n{tail}"
            )
        try:
            return make_client(args)
        except BaseException as exc:  # capture_client raises RuntimeError/OSError depending on transport.
            last_error = exc
            time.sleep(0.25)
    raise RuntimeError(f"timed out waiting for capture streams after {args.wait_streams_timeout_s}s: {last_error}")


def write_metadata(out: Path, args: argparse.Namespace, info: dict, frames: int, imu_count: int) -> None:
    data = {
        "created_at": datetime.now().isoformat(timespec="seconds"),
        "host": platform.node(),
        "platform": platform.platform(),
        "script": Path(__file__).name,
        "transport": resolve_transport(args.transport),
        "registry": args.registry,
        "tcp_host": args.tcp_host,
        "tcp_port": args.tcp_port,
        "capture_service_impl": args.capture_service_impl if args.start_capture_service else "external",
        "publish": args.publish if args.start_capture_service else "external",
        "package_root": str(default_package_root()) if args.start_capture_service and not args.package_root else args.package_root,
        "streams": {
            sid: {
                "width": int(s.width),
                "height": int(s.height),
                "format_name": s.format_name,
                "payload_size": int(s.payload_size),
                "kind": s.kind,
            }
            for sid, s in sorted(info.items())
        },
        "recorded_frames": frames,
        "recorded_imu_samples": imu_count,
        "stereo_max_delta_ms": args.stereo_max_delta_ms,
        "duration_requested_s": args.seconds,
    }
    (out / "record_metadata.json").write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")


def main() -> int:
    ap = argparse.ArgumentParser(description="Record XREAL unified 480x640 stereo+IMU Kalibr dataset")
    ap.add_argument("--transport", choices=["auto", "shm", "tcp"], default="auto")
    ap.add_argument("--registry", default=DEFAULT_REGISTRY)
    ap.add_argument("--tcp-host", default=DEFAULT_TCP_HOST)
    ap.add_argument("--tcp-port", type=int, default=DEFAULT_TCP_PORT)
    ap.add_argument("--tcp-bind-host", default="127.0.0.1", help="only used when --start-capture-service")
    ap.add_argument("--cam0-stream", default="camera0")
    ap.add_argument("--cam1-stream", default="camera1")
    ap.add_argument("--imu-stream", default="imu0")
    ap.add_argument("--seconds", type=float, default=90.0)
    ap.add_argument("--out-root", default=str(Path.home() / "xreal_records"))
    ap.add_argument("--name", default="")
    ap.add_argument("--stereo-max-delta-ms", type=float, default=1.0)
    ap.add_argument("--expect-width", type=int, default=480)
    ap.add_argument("--expect-height", type=int, default=640)
    ap.add_argument("--allow-size-mismatch", action="store_true")
    ap.add_argument("--warmup-seconds", type=float, default=2.0)
    ap.add_argument("--wait-streams-timeout-s", type=float, default=20.0)
    ap.add_argument("--sync-debug-every-s", type=float, default=2.0, help="print stream diagnostics while waiting for synced packets")

    ap.add_argument("--start-capture-service", action="store_true", help="start package capture_service before recording")
    ap.add_argument("--stop-capture-service", action="store_true", help="stop capture_service started by this script on exit")
    ap.add_argument("--package-root", default="", help="out/xreal_ultra package root; auto-detected by default")
    ap.add_argument("--capture-service-impl", choices=["cpp", "python"], default="cpp")
    ap.add_argument("--publish", default="shm", help="capture_service publish mode: shm, tcp, or shm,tcp")
    ap.add_argument("--capture-service-log", default="/tmp/xreal_record_capture_service.log")

    ap.add_argument("--left-rotate", default="", help="optional capture_service_cpp override: 0/180/cw90/ccw90")
    ap.add_argument("--right-rotate", default="", help="optional capture_service_cpp override: 0/180/cw90/ccw90")
    ap.add_argument("--left-flip", default="", help="optional capture_service_cpp override: none/x/y/xy")
    ap.add_argument("--right-flip", default="", help="optional capture_service_cpp override: none/x/y/xy")
    args = ap.parse_args()

    resolved_transport = resolve_transport(args.transport)
    if args.start_capture_service and resolved_transport == "tcp" and args.publish == "shm":
        # Avoid a common foot-gun when recording from the C++ service over TCP.
        args.publish = "tcp"
    if args.start_capture_service and resolved_transport == "shm" and "shm" not in args.publish.split(","):
        args.publish = "shm"

    stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    name = args.name or f"xreal_unified480_calib_{stamp}"
    out = expand_path(args.out_root) / name
    cam0_dir = out / "cam0" / "data"
    cam1_dir = out / "cam1" / "data"
    cam0_dir.mkdir(parents=True, exist_ok=True)
    cam1_dir.mkdir(parents=True, exist_ok=True)

    capture_proc: Optional[subprocess.Popen] = None
    client: Optional[CaptureClient] = None
    cam_csv = None
    imu_csv = None
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

        sync = BasaltStereoImuSynchronizer(
            client,
            cam0_stream=args.cam0_stream,
            cam1_stream=args.cam1_stream,
            imu_stream=args.imu_stream,
            stereo_max_delta_ns=int(args.stereo_max_delta_ms * 1e6),
            wait_for_imu_s=0.05,
        )

        cam_csv = open(out / "camera_timestamps.csv", "w", newline="", encoding="utf-8")
        imu_csv = open(out / "imu.csv", "w", newline="", encoding="utf-8")
        quality_csv = open(out / "frame_quality.csv", "w", newline="", encoding="utf-8")
        cam_w = csv.writer(cam_csv)
        imu_w = csv.writer(imu_csv)
        quality_w = csv.writer(quality_csv)

        cam_w.writerow(["timestamp_ns", "sequence", "cam0_file", "cam1_file", "cam0_ts_ns", "cam1_ts_ns", "stereo_delta_ns"])
        imu_w.writerow(["timestamp_ns", "gx", "gy", "gz", "ax", "ay", "az", "sequence"])
        quality_w.writerow(["timestamp_ns", "sequence", "cam0_mean", "cam0_std", "cam1_mean", "cam1_std"])

        start = time.monotonic()
        last_print = start
        last_no_packet_print = start
        no_packet_count = 0
        print(f"[record] writing Kalibr dataset: {out}", flush=True)
        print(f"[record] transport={resolved_transport} streams={args.cam0_stream},{args.cam1_stream},{args.imu_stream}", flush=True)

        while time.monotonic() - start < args.seconds:
            pkt = sync.read_next(timeout_s=1.0, copy_images=True)
            if pkt is None:
                no_packet_count += 1
                now = time.monotonic()
                if args.sync_debug_every_s > 0 and now - last_no_packet_print >= args.sync_debug_every_s:
                    print(f"[record][WARN] no synced stereo packet yet after {no_packet_count} waits: {_stream_debug_line(client, args)}", flush=True)
                    last_no_packet_print = now
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
            quality_w.writerow([
                ts,
                seq,
                float(img0.mean()),
                float(img0.std()),
                float(img1.mean()),
                float(img1.std()),
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

        quality_csv.close()
        if frames == 0:
            raise RuntimeError(
                "recorded zero frames. The stream registry was visible, but the Python synchronizer did not return "
                "any stereo packet. Check the [record][WARN] diagnostics above and /tmp/xreal_record_capture_service.log."
            )
    finally:
        if cam_csv:
            cam_csv.close()
        if imu_csv:
            imu_csv.close()
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
