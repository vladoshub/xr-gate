#!/usr/bin/env python3
"""XR startup gate for backend launch orchestration.

This module mirrors the Basalt backend startup visual/IMU gate logic, but keeps
it outside the VIO process so a future process supervisor can decide when to
start backends.

The implementation is intentionally split into transport-neutral gate logic and
capture_service client sources. Linux keeps the existing POSIX SHM path, while
Windows/native package runs can use the capture_service_cpp TCP path.
"""
from __future__ import annotations

import argparse
import math
import os
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, Optional, Protocol, Sequence


# Allow running directly from the source tree without installing capture_service.
# This copy is owned by xr_client and is expected at:
#   <root>/xr_client/tools/xr_startup_gate.py
# Keep root detection layout-neutral so the client is not tied to a backend path.
def _find_project_root() -> Path:
    here = Path(__file__).resolve()
    for parent in [here.parent, *here.parents]:
        if (parent / "capture_service").exists():
            return parent
    # Fallback for the expected <root>/xr_client/tools layout.
    return here.parents[2]


PROJECT_ROOT = _find_project_root()


def _prepend_sys_path(path: Path) -> None:
    try:
        resolved = path.resolve()
    except Exception:
        resolved = path
    text = str(resolved)
    if path.exists() and text not in sys.path:
        sys.path.insert(0, text)


def _install_capture_client_import_paths() -> None:
    candidates = []
    for env_name in ("XR_PACKAGE_ROOT", "XR_ROOT_PROJECT", "ROOT_PROJECT"):
        value = os.environ.get(env_name)
        if not value:
            continue
        root = Path(value).expanduser()
        candidates.extend([
            root / "bin" / "python",
            root / "bin" / "python" / "capture_service",
            root / "capture_service",
            root,
        ])
    candidates.extend([
        PROJECT_ROOT / "bin" / "python",
        PROJECT_ROOT / "bin" / "python" / "capture_service",
        PROJECT_ROOT / "capture_service",
        PROJECT_ROOT,
    ])
    for candidate in candidates:
        if (candidate / "capture_client").exists():
            _prepend_sys_path(candidate)


def _load_capture_client_modules():
    _install_capture_client_import_paths()
    try:
        from capture_client import BasaltStereoImuSynchronizer, CaptureClient, StereoPairReader  # type: ignore
    except Exception as exc:  # pragma: no cover - depends on deployment layout
        raise RuntimeError(
            "failed to import capture_client; run from xr_tracking root, set ROOT_PROJECT, "
            "or add bin/python / project root to PYTHONPATH"
        ) from exc
    return CaptureClient, BasaltStereoImuSynchronizer, StereoPairReader


@dataclass(frozen=True)
class ImageHealth:
    mean: float = 0.0
    stddev: float = 0.0
    black_fraction: float = 1.0
    white_fraction: float = 0.0
    corners: int = 0
    grid_cells: int = 0
    laplacian_stddev: float = 0.0


@dataclass(frozen=True)
class ImuHealth:
    samples: int = 0
    gyro_norm_mean: float = 0.0
    gyro_norm_stddev: float = 0.0
    gyro_norm_max: float = 0.0
    accel_norm_mean: float = 0.0
    accel_norm_stddev: float = 0.0
    accel_norm_max: float = 0.0
    accel_magnitude_error: float = math.inf


@dataclass(frozen=True)
class VisualGateConfig:
    enabled: bool = False
    good_frames: int = 20
    min_mean: float = 14.0
    min_stddev: float = 6.0
    max_black_fraction: float = 0.85
    max_white_fraction: float = 0.25
    min_corners: int = 80
    min_grid_cells: int = 8
    min_laplacian_stddev: float = 8.0


@dataclass(frozen=True)
class StreamQualityGateConfig:
    enabled: bool = False
    window_s: float = 5.0
    max_defective_frames: int = 5
    min_frames: int = 20
    min_mean: float = 12.0
    min_stddev: float = 4.0
    max_black_fraction: float = 0.90
    min_corners: int = 4
    min_laplacian_stddev: float = 3.0


@dataclass(frozen=True)
class ImuGateConfig:
    enabled: bool = False
    good_frames: int = 30
    min_samples: int = 10
    max_gyro_norm: float = 0.08
    max_gyro_stddev: float = 0.04
    max_accel_magnitude_error: float = 0.75
    max_accel_stddev: float = 0.35
    expected_gravity_magnitude: float = 9.80665


@dataclass(frozen=True)
class StartupGateConfig:
    visual: VisualGateConfig = VisualGateConfig()
    imu: ImuGateConfig = ImuGateConfig()
    quality: StreamQualityGateConfig = StreamQualityGateConfig()
    print_every: int = 30
    timeout_s: float = 0.0
    read_timeout_s: float = 1.0


@dataclass
class StartupGateResult:
    passed: bool
    visual_ready: bool
    imu_ready: bool
    frames_seen: int
    visual_seen: int
    visual_good: int
    imu_seen: int
    imu_good: int
    quality_seen: int = 0
    quality_defective: int = 0
    quality_window_s: float = 0.0
    reason: str = ""


class SyncedPacketSource(Protocol):
    def read_next(self, timeout_s: float):
        ...

    def close(self) -> None:
        ...


class CaptureServiceStereoImuSource:
    """Stereo+IMU source backed by capture_client over SHM or TCP."""

    def __init__(
        self,
        *,
        transport: str,
        registry: str,
        tcp_host: str,
        tcp_port: int,
        cam0_stream: str,
        cam1_stream: str,
        imu_stream: str,
        stereo_max_delta_ms: float,
        wait_for_imu_s: float,
    ) -> None:
        CaptureClient, BasaltStereoImuSynchronizer, _StereoPairReader = _load_capture_client_modules()
        if transport == "shm":
            if os.name != "posix":
                raise RuntimeError("capture_service SHM startup gate is only supported on POSIX; use --transport tcp on Windows")
            self.client = CaptureClient.from_shm_registry(registry, required_streams=[cam0_stream, cam1_stream, imu_stream])
        elif transport == "tcp":
            self.client = CaptureClient.from_tcp(
                tcp_host,
                int(tcp_port),
                required_streams=[cam0_stream, cam1_stream, imu_stream],
                subscribe_streams=[cam0_stream, cam1_stream, imu_stream],
            )
        else:
            raise RuntimeError(f"unsupported capture transport: {transport}")
        self.sync = BasaltStereoImuSynchronizer(
            self.client,
            cam0_stream=cam0_stream,
            cam1_stream=cam1_stream,
            imu_stream=imu_stream,
            stereo_max_delta_ns=int(stereo_max_delta_ms * 1_000_000.0),
            wait_for_imu_s=wait_for_imu_s,
        )

    def read_next(self, timeout_s: float):
        return self.sync.read_next(timeout_s=timeout_s, copy_images=True)

    def close(self) -> None:
        self.client.close()


class CaptureServiceStereoSource:
    """Camera-only source used by the stream-quality preflight gate."""

    def __init__(
        self,
        *,
        transport: str,
        registry: str,
        tcp_host: str,
        tcp_port: int,
        cam0_stream: str,
        cam1_stream: str,
        stereo_max_delta_ms: float,
    ) -> None:
        CaptureClient, _BasaltStereoImuSynchronizer, StereoPairReader = _load_capture_client_modules()
        if transport == "shm":
            if os.name != "posix":
                raise RuntimeError("capture_service SHM startup gate is only supported on POSIX; use --transport tcp on Windows")
            self.client = CaptureClient.from_shm_registry(registry, required_streams=[cam0_stream, cam1_stream])
        elif transport == "tcp":
            self.client = CaptureClient.from_tcp(
                tcp_host,
                int(tcp_port),
                required_streams=[cam0_stream, cam1_stream],
                subscribe_streams=[cam0_stream, cam1_stream],
            )
        else:
            raise RuntimeError(f"unsupported capture transport: {transport}")
        self.pairs = StereoPairReader(
            self.client,
            cam0_stream=cam0_stream,
            cam1_stream=cam1_stream,
            max_timestamp_delta_ns=int(stereo_max_delta_ms * 1_000_000.0),
        )

    def read_next(self, timeout_s: float):
        return self.pairs.read_next_pair(timeout_s=timeout_s, copy_payload=True)

    def close(self) -> None:
        self.client.close()


# Compatibility aliases for older imports/tests.
CaptureServicePosixShmSource = CaptureServiceStereoImuSource
CaptureServicePosixStereoSource = CaptureServiceStereoSource

def _require_cv2_np():
    try:
        import cv2  # type: ignore
        import numpy as np  # type: ignore
    except Exception as exc:  # pragma: no cover - environment-dependent
        raise RuntimeError(
            "visual startup gate requires python OpenCV and numpy; install python3-opencv/python3-numpy "
            "or disable --visual-gate"
        ) from exc
    return cv2, np


def compute_image_health(frame) -> ImageHealth:
    """Compute the same visual health metrics as capture_basalt_backend.cpp."""

    width = int(getattr(frame, "width", 0) or 0)
    height = int(getattr(frame, "height", 0) or 0)
    payload = getattr(frame, "data", b"") or b""
    if width <= 0 or height <= 0 or not payload:
        return ImageHealth()

    fmt = str(getattr(frame, "format_name", "") or "").upper()
    if fmt != "GRAY8":
        raise RuntimeError(f"visual startup gate expects GRAY8 frames, got {fmt!r}")

    cv2, np = _require_cv2_np()
    arr = np.frombuffer(payload, dtype=np.uint8)
    expected = width * height
    if arr.size < expected:
        return ImageHealth()
    if arr.size != expected:
        arr = arr[:expected]
    gray = arr.reshape((height, width))

    mean = float(gray.mean())
    stddev = float(gray.std())
    black_fraction = float((gray <= 8).sum()) / float(expected)
    white_fraction = float((gray >= 248).sum()) / float(expected)

    corners = cv2.goodFeaturesToTrack(gray, maxCorners=800, qualityLevel=0.01, minDistance=7.0)
    corners_count = 0 if corners is None else int(len(corners))

    grid_cols = 4
    grid_rows = 4
    occupied = [[False for _ in range(grid_cols)] for _ in range(grid_rows)]
    if corners is not None:
        for pt in corners.reshape((-1, 2)):
            gx = max(0, min(grid_cols - 1, int(float(pt[0]) * grid_cols / max(1, width))))
            gy = max(0, min(grid_rows - 1, int(float(pt[1]) * grid_rows / max(1, height))))
            occupied[gy][gx] = True
    grid_cells = sum(1 for row in occupied for value in row if value)

    lap = cv2.Laplacian(gray, cv2.CV_64F)
    laplacian_stddev = float(lap.std())

    return ImageHealth(
        mean=mean,
        stddev=stddev,
        black_fraction=black_fraction,
        white_fraction=white_fraction,
        corners=corners_count,
        grid_cells=grid_cells,
        laplacian_stddev=laplacian_stddev,
    )


def image_health_ok(h: ImageHealth, cfg: VisualGateConfig) -> bool:
    return (
        h.mean >= cfg.min_mean
        and h.stddev >= cfg.min_stddev
        and h.black_fraction <= cfg.max_black_fraction
        and h.white_fraction <= cfg.max_white_fraction
        and h.corners >= cfg.min_corners
        and h.grid_cells >= cfg.min_grid_cells
        and h.laplacian_stddev >= cfg.min_laplacian_stddev
    )


def image_health_string(h: ImageHealth) -> str:
    return (
        f"mean={h.mean:.2f} std={h.stddev:.2f} black={h.black_fraction:.2f} "
        f"white={h.white_fraction:.2f} corners={h.corners} grid={h.grid_cells} "
        f"lap_std={h.laplacian_stddev:.2f}"
    )


def compute_imu_health(samples: Sequence, expected_gravity_magnitude: float) -> ImuHealth:
    count = len(samples)
    if count <= 0:
        return ImuHealth()

    gyro_values = []
    accel_values = []
    for sample in samples:
        gx, gy, gz = sample.gyro_rad_s
        ax, ay, az = sample.accel_m_s2
        gyro_values.append(math.sqrt(gx * gx + gy * gy + gz * gz))
        accel_values.append(math.sqrt(ax * ax + ay * ay + az * az))

    def mean(xs: Sequence[float]) -> float:
        return sum(xs) / float(len(xs))

    def stddev(xs: Sequence[float], m: float) -> float:
        return math.sqrt(max(0.0, sum(x * x for x in xs) / float(len(xs)) - m * m))

    gyro_mean = mean(gyro_values)
    accel_mean = mean(accel_values)
    return ImuHealth(
        samples=count,
        gyro_norm_mean=gyro_mean,
        gyro_norm_stddev=stddev(gyro_values, gyro_mean),
        gyro_norm_max=max(gyro_values),
        accel_norm_mean=accel_mean,
        accel_norm_stddev=stddev(accel_values, accel_mean),
        accel_norm_max=max(accel_values),
        accel_magnitude_error=abs(accel_mean - expected_gravity_magnitude),
    )


def imu_health_ok(h: ImuHealth, cfg: ImuGateConfig) -> bool:
    return (
        h.samples >= max(0, cfg.min_samples)
        and h.gyro_norm_mean <= cfg.max_gyro_norm
        and h.gyro_norm_stddev <= cfg.max_gyro_stddev
        and h.accel_magnitude_error <= cfg.max_accel_magnitude_error
        and h.accel_norm_stddev <= cfg.max_accel_stddev
    )


def imu_health_string(h: ImuHealth) -> str:
    return (
        f"samples={h.samples} gyro_mean={h.gyro_norm_mean:.4f} "
        f"gyro_std={h.gyro_norm_stddev:.4f} gyro_max={h.gyro_norm_max:.4f} "
        f"accel_mean={h.accel_norm_mean:.4f} accel_std={h.accel_norm_stddev:.4f} "
        f"accel_err={h.accel_magnitude_error:.4f}"
    )


def image_defective(h: ImageHealth, cfg: StreamQualityGateConfig) -> bool:
    # Defective here means a stream-session failure, not merely a visually weak
    # tracking frame. It catches fully/near-black frames and frames with almost
    # no image detail. Thresholds are intentionally separate from the stricter
    # Basalt startup visual gate.
    if h.mean <= cfg.min_mean:
        return True
    if h.black_fraction >= cfg.max_black_fraction:
        return True
    no_detail = (
        h.stddev <= cfg.min_stddev
        and h.laplacian_stddev <= cfg.min_laplacian_stddev
        and h.corners <= cfg.min_corners
    )
    return bool(no_detail)


def run_stream_quality_gate(source: SyncedPacketSource, cfg: StartupGateConfig) -> StartupGateResult:
    quality = cfg.quality
    if not quality.enabled:
        return StartupGateResult(True, True, True, 0, 0, 0, 0, 0, reason="quality-disabled")
    if quality.window_s <= 0.0:
        raise ValueError("quality window_s must be positive")
    if quality.max_defective_frames < 0:
        raise ValueError("quality max_defective_frames must be non-negative")

    wall_start = time.monotonic()
    last_print_seen = -1
    frames_seen = 0
    defective = 0

    while True:
        elapsed_s = time.monotonic() - wall_start
        if elapsed_s >= quality.window_s:
            passed = frames_seen >= quality.min_frames and defective <= quality.max_defective_frames
            reason = "passed" if passed else (
                "not-enough-frames" if frames_seen < quality.min_frames else "too-many-defective-frames"
            )
            print(
                "[xr_startup_gate] stream quality gate result: "
                f"elapsed={elapsed_s:.2f}s seen={frames_seen} "
                f"defective={defective}/{quality.max_defective_frames} "
                f"min_frames={quality.min_frames} passed={str(passed).lower()} reason={reason}",
                flush=True,
            )
            return StartupGateResult(
                passed=passed,
                visual_ready=passed,
                imu_ready=True,
                frames_seen=frames_seen,
                visual_seen=frames_seen,
                visual_good=max(0, frames_seen - defective),
                imu_seen=0,
                imu_good=0,
                quality_seen=frames_seen,
                quality_defective=defective,
                quality_window_s=elapsed_s,
                reason=reason,
            )

        packet = source.read_next(timeout_s=min(cfg.read_timeout_s, max(0.05, quality.window_s - elapsed_s)))
        if packet is None:
            print("[xr_startup_gate] WARN: stream quality sync timeout", flush=True)
            continue

        pair = getattr(packet, "pair", packet)
        frames_seen += 1
        h0 = compute_image_health(pair.cam0)
        h1 = compute_image_health(pair.cam1)
        bad0 = image_defective(h0, quality)
        bad1 = image_defective(h1, quality)
        bad = bad0 or bad1
        if bad:
            defective += 1

        should_print = (
            frames_seen == 1
            or bad
            or (cfg.print_every > 0 and frames_seen % cfg.print_every == 0 and frames_seen != last_print_seen)
        )
        if should_print:
            last_print_seen = frames_seen
            elapsed_s = time.monotonic() - wall_start
            print(
                "[xr_startup_gate] stream quality gate: "
                f"elapsed={elapsed_s:.2f}/{quality.window_s:.2f}s seen={frames_seen} "
                f"defective={defective}/{quality.max_defective_frames} "
                f"cam0{{{image_health_string(h0)}}} defective0={str(bad0).lower()} "
                f"cam1{{{image_health_string(h1)}}} defective1={str(bad1).lower()}",
                flush=True,
            )


def validate_config(cfg: StartupGateConfig) -> None:
    if cfg.visual.good_frames <= 0:
        raise ValueError("visual good_frames must be positive")
    if cfg.imu.good_frames <= 0:
        raise ValueError("IMU good_frames must be positive")
    if cfg.quality.window_s <= 0.0:
        raise ValueError("stream quality window must be positive")
    if cfg.quality.min_frames < 0:
        raise ValueError("stream quality min_frames must be non-negative")
    if cfg.imu.min_samples < 0:
        raise ValueError("IMU min_samples must be non-negative")
    values = [
        cfg.visual.min_mean,
        cfg.visual.min_stddev,
        cfg.visual.max_black_fraction,
        cfg.visual.max_white_fraction,
        cfg.visual.min_laplacian_stddev,
        cfg.imu.max_gyro_norm,
        cfg.imu.max_gyro_stddev,
        cfg.imu.max_accel_magnitude_error,
        cfg.imu.max_accel_stddev,
        cfg.imu.expected_gravity_magnitude,
    ]
    if any((not math.isfinite(v) or v < 0.0) for v in values):
        raise ValueError("startup gate thresholds must be finite non-negative numbers")


def run_startup_gate(source: SyncedPacketSource, cfg: StartupGateConfig) -> StartupGateResult:
    validate_config(cfg)

    visual_ready = not cfg.visual.enabled
    imu_ready = not cfg.imu.enabled
    visual_good = 0
    imu_good = 0
    visual_seen = 0
    imu_seen = 0
    frames_seen = 0
    wall_start = time.monotonic()

    while True:
        if cfg.timeout_s > 0.0 and time.monotonic() - wall_start >= cfg.timeout_s:
            return StartupGateResult(
                passed=False,
                visual_ready=visual_ready,
                imu_ready=imu_ready,
                frames_seen=frames_seen,
                visual_seen=visual_seen,
                visual_good=visual_good,
                imu_seen=imu_seen,
                imu_good=imu_good,
                reason="timeout",
            )

        packet = source.read_next(timeout_s=cfg.read_timeout_s)
        if packet is None:
            print("[xr_startup_gate] WARN: sync timeout", flush=True)
            continue
        frames_seen += 1

        if not getattr(packet, "imu_samples", ()):  # same warning as backend gate path.
            print(
                f"[xr_startup_gate] WARN: empty IMU window at frame timestamp {packet.pair.timestamp_ns}",
                flush=True,
            )

        if not visual_ready:
            h0 = compute_image_health(packet.pair.cam0)
            h1 = compute_image_health(packet.pair.cam1)
            ok0 = image_health_ok(h0, cfg.visual)
            ok1 = image_health_ok(h1, cfg.visual)
            ok = ok0 and ok1
            visual_seen += 1
            visual_good = visual_good + 1 if ok else 0

            if (
                visual_seen == 1
                or visual_good >= cfg.visual.good_frames
                or (cfg.print_every > 0 and visual_seen % cfg.print_every == 0)
            ):
                print(
                    "[xr_startup_gate] startup visual gate: "
                    f"seen={visual_seen} good={visual_good}/{cfg.visual.good_frames} "
                    f"cam0{{{image_health_string(h0)}}} ok0={str(ok0).lower()} "
                    f"cam1{{{image_health_string(h1)}}} ok1={str(ok1).lower()}",
                    flush=True,
                )

            if visual_good >= cfg.visual.good_frames:
                visual_ready = True
                print(
                    "[xr_startup_gate] startup visual gate passed at frame timestamp "
                    f"{packet.pair.timestamp_ns}",
                    flush=True,
                )

        if not imu_ready:
            h = compute_imu_health(packet.imu_samples, abs(cfg.imu.expected_gravity_magnitude))
            ok = imu_health_ok(h, cfg.imu)
            imu_seen += 1
            imu_good = imu_good + 1 if ok else 0

            if (
                imu_seen == 1
                or imu_good >= cfg.imu.good_frames
                or (cfg.print_every > 0 and imu_seen % cfg.print_every == 0)
            ):
                print(
                    "[xr_startup_gate] startup IMU gate: "
                    f"seen={imu_seen} good={imu_good}/{cfg.imu.good_frames} "
                    f"imu{{{imu_health_string(h)}}} ok={str(ok).lower()}",
                    flush=True,
                )

            if imu_good >= cfg.imu.good_frames:
                imu_ready = True
                print(
                    "[xr_startup_gate] startup IMU gate passed at frame timestamp "
                    f"{packet.pair.timestamp_ns}",
                    flush=True,
                )

        if visual_ready and imu_ready:
            return StartupGateResult(
                passed=True,
                visual_ready=True,
                imu_ready=True,
                frames_seen=frames_seen,
                visual_seen=visual_seen,
                visual_good=visual_good,
                imu_seen=imu_seen,
                imu_good=imu_good,
                reason="passed",
            )


def _add_bool_gate_flags(ap: argparse.ArgumentParser) -> None:
    ap.add_argument("--visual-gate", action="store_true", help="Enable visual startup gate")
    ap.add_argument("--imu-gate", action="store_true", help="Enable IMU startup-at-rest gate")


def parse_args(argv: Optional[Sequence[str]] = None) -> argparse.Namespace:
    ap = argparse.ArgumentParser(description="Wait until capture_service streams pass XR startup visual/IMU gates.")
    ap.add_argument("--transport", choices=["auto", "shm", "tcp"], default="auto", help="Input transport; auto=shm on POSIX, tcp on Windows")
    ap.add_argument("--registry", default="/tmp/capture_service_streams.json")
    ap.add_argument("--tcp-host", default="127.0.0.1")
    ap.add_argument("--tcp-port", type=int, default=45660)
    ap.add_argument("--cam0-stream", default="camera0")
    ap.add_argument("--cam1-stream", default="camera1")
    ap.add_argument("--imu-stream", default="imu0")
    ap.add_argument("--stereo-max-delta-ms", type=float, default=1.0)
    ap.add_argument("--wait-for-imu-s", type=float, default=0.05)
    ap.add_argument("--read-timeout-s", type=float, default=1.0)
    ap.add_argument("--timeout-s", type=float, default=0.0, help="Overall timeout; 0 means wait forever")
    ap.add_argument("--print-every", type=int, default=30)
    _add_bool_gate_flags(ap)
    ap.add_argument("--stream-quality-gate", action="store_true", help="Run a fixed-window camera quality preflight gate")
    ap.add_argument("--quality-window-s", type=float, default=5.0)
    ap.add_argument("--quality-max-defective-frames", type=int, default=5)
    ap.add_argument("--quality-min-frames", type=int, default=20)
    ap.add_argument("--quality-min-mean", type=float, default=12.0)
    ap.add_argument("--quality-min-stddev", type=float, default=4.0)
    ap.add_argument("--quality-max-black-fraction", type=float, default=0.90)
    ap.add_argument("--quality-min-corners", type=int, default=4)
    ap.add_argument("--quality-min-laplacian-stddev", type=float, default=3.0)

    ap.add_argument("--visual-good-frames", type=int, default=20)
    ap.add_argument("--min-mean", type=float, default=14.0)
    ap.add_argument("--min-stddev", type=float, default=6.0)
    ap.add_argument("--max-black-fraction", type=float, default=0.85)
    ap.add_argument("--max-white-fraction", type=float, default=0.25)
    ap.add_argument("--min-corners", type=int, default=80)
    ap.add_argument("--min-grid-cells", type=int, default=8)
    ap.add_argument("--min-laplacian-stddev", type=float, default=8.0)

    ap.add_argument("--imu-good-frames", type=int, default=30)
    ap.add_argument("--imu-min-samples", type=int, default=10)
    ap.add_argument("--imu-max-gyro-norm", type=float, default=0.08)
    ap.add_argument("--imu-max-gyro-stddev", type=float, default=0.04)
    ap.add_argument("--imu-max-accel-magnitude-error", type=float, default=0.75)
    ap.add_argument("--imu-max-accel-stddev", type=float, default=0.35)
    ap.add_argument("--gravity-magnitude", type=float, default=9.80665)
    return ap.parse_args(argv)


def config_from_args(args: argparse.Namespace) -> StartupGateConfig:
    return StartupGateConfig(
        visual=VisualGateConfig(
            enabled=bool(args.visual_gate),
            good_frames=args.visual_good_frames,
            min_mean=args.min_mean,
            min_stddev=args.min_stddev,
            max_black_fraction=args.max_black_fraction,
            max_white_fraction=args.max_white_fraction,
            min_corners=args.min_corners,
            min_grid_cells=args.min_grid_cells,
            min_laplacian_stddev=args.min_laplacian_stddev,
        ),
        quality=StreamQualityGateConfig(
            enabled=bool(args.stream_quality_gate),
            window_s=args.quality_window_s,
            max_defective_frames=args.quality_max_defective_frames,
            min_frames=args.quality_min_frames,
            min_mean=args.quality_min_mean,
            min_stddev=args.quality_min_stddev,
            max_black_fraction=args.quality_max_black_fraction,
            min_corners=args.quality_min_corners,
            min_laplacian_stddev=args.quality_min_laplacian_stddev,
        ),
        imu=ImuGateConfig(
            enabled=bool(args.imu_gate),
            good_frames=args.imu_good_frames,
            min_samples=args.imu_min_samples,
            max_gyro_norm=args.imu_max_gyro_norm,
            max_gyro_stddev=args.imu_max_gyro_stddev,
            max_accel_magnitude_error=args.imu_max_accel_magnitude_error,
            max_accel_stddev=args.imu_max_accel_stddev,
            expected_gravity_magnitude=args.gravity_magnitude,
        ),
        print_every=args.print_every,
        timeout_s=args.timeout_s,
        read_timeout_s=args.read_timeout_s,
    )


def resolve_transport(value: str) -> str:
    transport = str(value).strip().lower()
    if transport == "auto":
        return "tcp" if os.name == "nt" else "shm"
    return transport


def main(argv: Optional[Sequence[str]] = None) -> int:
    args = parse_args(argv)
    cfg = config_from_args(args)
    transport = resolve_transport(args.transport)

    print(f"[xr_startup_gate] transport={transport}", flush=True)
    if transport == "shm":
        print(f"[xr_startup_gate] registry={args.registry}", flush=True)
    else:
        print(f"[xr_startup_gate] tcp={args.tcp_host}:{args.tcp_port}", flush=True)
    print(f"[xr_startup_gate] streams cam0={args.cam0_stream} cam1={args.cam1_stream} imu={args.imu_stream}", flush=True)
    if cfg.visual.enabled:
        print(
            "[xr_startup_gate] startup_visual_gate: enabled "
            f"good_frames={cfg.visual.good_frames} min_mean={cfg.visual.min_mean} "
            f"min_stddev={cfg.visual.min_stddev} max_black_fraction={cfg.visual.max_black_fraction} "
            f"max_white_fraction={cfg.visual.max_white_fraction} min_corners={cfg.visual.min_corners} "
            f"min_grid_cells={cfg.visual.min_grid_cells} min_laplacian_stddev={cfg.visual.min_laplacian_stddev}",
            flush=True,
        )
    else:
        print("[xr_startup_gate] startup_visual_gate: disabled", flush=True)
    if cfg.quality.enabled:
        print(
            "[xr_startup_gate] stream_quality_gate: enabled "
            f"window_s={cfg.quality.window_s} max_defective_frames={cfg.quality.max_defective_frames} "
            f"min_frames={cfg.quality.min_frames} min_mean={cfg.quality.min_mean} "
            f"min_stddev={cfg.quality.min_stddev} max_black_fraction={cfg.quality.max_black_fraction} "
            f"min_corners={cfg.quality.min_corners} min_laplacian_stddev={cfg.quality.min_laplacian_stddev}",
            flush=True,
        )
    else:
        print("[xr_startup_gate] stream_quality_gate: disabled", flush=True)
    if cfg.imu.enabled:
        print(
            "[xr_startup_gate] startup_imu_gate: enabled "
            f"good_frames={cfg.imu.good_frames} min_samples={cfg.imu.min_samples} "
            f"max_gyro_norm={cfg.imu.max_gyro_norm} max_gyro_stddev={cfg.imu.max_gyro_stddev} "
            f"max_accel_magnitude_error={cfg.imu.max_accel_magnitude_error} "
            f"max_accel_stddev={cfg.imu.max_accel_stddev} gravity_magnitude={cfg.imu.expected_gravity_magnitude}",
            flush=True,
        )
    else:
        print("[xr_startup_gate] startup_imu_gate: disabled", flush=True)

    source: Optional[object] = None
    try:
        if cfg.quality.enabled and not cfg.visual.enabled and not cfg.imu.enabled:
            source = CaptureServiceStereoSource(
                transport=transport,
                registry=args.registry,
                tcp_host=args.tcp_host,
                tcp_port=args.tcp_port,
                cam0_stream=args.cam0_stream,
                cam1_stream=args.cam1_stream,
                stereo_max_delta_ms=args.stereo_max_delta_ms,
            )
            result = run_stream_quality_gate(source, cfg)
        else:
            source = CaptureServiceStereoImuSource(
                transport=transport,
                registry=args.registry,
                tcp_host=args.tcp_host,
                tcp_port=args.tcp_port,
                cam0_stream=args.cam0_stream,
                cam1_stream=args.cam1_stream,
                imu_stream=args.imu_stream,
                stereo_max_delta_ms=args.stereo_max_delta_ms,
                wait_for_imu_s=args.wait_for_imu_s,
            )
            if cfg.quality.enabled:
                q_source = CaptureServiceStereoSource(
                    transport=transport,
                    registry=args.registry,
                    tcp_host=args.tcp_host,
                    tcp_port=args.tcp_port,
                    cam0_stream=args.cam0_stream,
                    cam1_stream=args.cam1_stream,
                    stereo_max_delta_ms=args.stereo_max_delta_ms,
                )
                try:
                    q_result = run_stream_quality_gate(q_source, cfg)
                finally:
                    q_source.close()
                if not q_result.passed:
                    result = q_result
                else:
                    result = run_startup_gate(source, cfg)
            else:
                result = run_startup_gate(source, cfg)
    except KeyboardInterrupt:
        print("[xr_startup_gate] interrupted", file=sys.stderr, flush=True)
        return 130
    except Exception as exc:
        print(f"[xr_startup_gate][ERROR] {exc}", file=sys.stderr, flush=True)
        return 1

    if not result.passed:
        print(f"[xr_startup_gate][ERROR] startup gate failed: {result.reason}", file=sys.stderr, flush=True)
        try:
            if source is not None:
                source.close()
        except Exception:
            pass
        return 2

    print(
        "[xr_startup_gate] startup gate passed: "
        f"frames={result.frames_seen} visual_good={result.visual_good} imu_good={result.imu_good}",
        flush=True,
    )
    # Success is the common path for this short-lived helper. Do not call
    # CaptureClient.close() here: during backend restarts it can block while the
    # parent client is waiting for this process to exit. The process is about to
    # terminate, so the OS will release SHM/file descriptors safely.
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
