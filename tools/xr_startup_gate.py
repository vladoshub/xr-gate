#!/usr/bin/env python3
"""XR startup gate for backend launch orchestration.

This module mirrors the Basalt backend startup visual/IMU gate logic, but keeps
it outside the VIO process so a future process supervisor can decide when to
start backends.

The implementation is intentionally split into transport-neutral gate logic and
an initial POSIX/capture_service SHM source. A Windows/TCP source can be added
later without changing the gate thresholds/state machine.
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
# File path: <root>/tools/xr_startup_gate.py
PROJECT_ROOT = Path(__file__).resolve().parents[4]
CAPTURE_SERVICE_ROOT = PROJECT_ROOT / "capture_service"
if CAPTURE_SERVICE_ROOT.exists():
    sys.path.insert(0, str(CAPTURE_SERVICE_ROOT))


def _load_capture_client_modules():
    try:
        from capture_client import BasaltStereoImuSynchronizer, CaptureClient  # type: ignore
    except Exception as exc:  # pragma: no cover - depends on deployment layout
        raise RuntimeError(
            "failed to import capture_client; run from xr_tracking root or set "
            "PYTHONPATH=$ROOT_PROJECT"
        ) from exc
    return CaptureClient, BasaltStereoImuSynchronizer


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
    reason: str = ""


class SyncedPacketSource(Protocol):
    def read_next(self, timeout_s: float):
        ...

    def close(self) -> None:
        ...


class CaptureServicePosixShmSource:
    """Initial POSIX implementation backed by capture_service SHM registry."""

    def __init__(
        self,
        *,
        registry: str,
        cam0_stream: str,
        cam1_stream: str,
        imu_stream: str,
        stereo_max_delta_ms: float,
        wait_for_imu_s: float,
    ) -> None:
        if os.name != "posix":
            raise RuntimeError("capture_service SHM startup gate is currently implemented only for POSIX")
        CaptureClient, BasaltStereoImuSynchronizer = _load_capture_client_modules()
        self.client = CaptureClient.from_shm_registry(registry, required_streams=[cam0_stream, cam1_stream, imu_stream])
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


def validate_config(cfg: StartupGateConfig) -> None:
    if cfg.visual.good_frames <= 0:
        raise ValueError("visual good_frames must be positive")
    if cfg.imu.good_frames <= 0:
        raise ValueError("IMU good_frames must be positive")
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
    ap.add_argument("--transport", choices=["shm"], default="shm", help="Input transport; only POSIX SHM is implemented now")
    ap.add_argument("--registry", default="/tmp/capture_service_streams.json")
    ap.add_argument("--cam0-stream", default="camera0")
    ap.add_argument("--cam1-stream", default="camera1")
    ap.add_argument("--imu-stream", default="imu0")
    ap.add_argument("--stereo-max-delta-ms", type=float, default=1.0)
    ap.add_argument("--wait-for-imu-s", type=float, default=0.05)
    ap.add_argument("--read-timeout-s", type=float, default=1.0)
    ap.add_argument("--timeout-s", type=float, default=0.0, help="Overall timeout; 0 means wait forever")
    ap.add_argument("--print-every", type=int, default=30)
    _add_bool_gate_flags(ap)

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


def main(argv: Optional[Sequence[str]] = None) -> int:
    args = parse_args(argv)
    cfg = config_from_args(args)

    print("[xr_startup_gate] transport=shm(posix)", flush=True)
    print(f"[xr_startup_gate] registry={args.registry}", flush=True)
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

    source: Optional[CaptureServicePosixShmSource] = None
    try:
        source = CaptureServicePosixShmSource(
            registry=args.registry,
            cam0_stream=args.cam0_stream,
            cam1_stream=args.cam1_stream,
            imu_stream=args.imu_stream,
            stereo_max_delta_ms=args.stereo_max_delta_ms,
            wait_for_imu_s=args.wait_for_imu_s,
        )
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
    # Success is the common path for this short-lived helper.  Do not call
    # CaptureClient.close() here: during backend restarts it can block while the
    # parent client is waiting for this process to exit.  The process is about to
    # terminate, so the OS will release SHM/file descriptors safely.
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
