#!/usr/bin/env python3
"""
Universal guided IMU axis diagnostic for XR Gate controller_input V3 SHM.

The script works with any IMU provider that publishes gyroscope samples into
the left or right controller slot of the controller_input V3 shared-memory
stream.

XR Gate runtime_local / OpenXR aim-pose target convention:
  +X = right
  +Y = up
  -Z = forward

Expected positive physical motions used by this test:
  pitch up  -> +X
  yaw right -> -Y
  roll right -> -Z

Before running the test, disable any upstream orientation_transform for the
selected side and restart the component that publishes controller_input.

Examples:
  python3 calibrate_imu_axes.py left
  python3 calibrate_imu_axes.py right

The recording lasts 30 seconds and produces both a human-readable summary and
a JSON report with a suggested XR Gate orientation_transform.
"""

from __future__ import annotations

import argparse
import json
import math
import mmap
import os
import struct
import sys
import time
from collections import deque
from dataclasses import dataclass, field
from pathlib import Path
from typing import Deque, Dict, List, Optional, Sequence, Set, Tuple


# controller_input V3 packed ABI.
RING_HEADER_STRUCT = struct.Struct("<8s7IQ")
SLOT_HEADER_STRUCT = struct.Struct("<QQQQII")

FRAME_HEADER_SIZE = 56
V3_FRAME_SIZE = 1432
V3_SIDE_SIZE = 688
V3_IMU_OFFSET = 400

IMU_STATUS_ACTIVE = 3
IMU_SAMPLE_SIZE = 48
IMU_SAMPLES_OFFSET = 80

AXIS_NAMES = ("X", "Y", "Z")
REPORT_FORMAT = "IMU_AXIS_DIAG_V1"
RECORDING_DURATION_SEC = 30.0


@dataclass(frozen=True)
class RegistryInfo:
    shm_name: str
    header_size: int
    slot_count: int
    slot_stride: int
    slot_header_size: int
    payload_size: int


@dataclass(frozen=True)
class ImuSample:
    timestamp_ns: int
    device_timestamp_ticks: int
    gyro: Tuple[float, float, float]
    accel: Tuple[float, float, float]
    flags: int


@dataclass(frozen=True)
class ImuState:
    status: int
    data_flags: int
    sequence: int
    orientation_xyzw: Tuple[float, float, float, float]
    samples: List[ImuSample]


@dataclass
class PhaseStats:
    name: str
    instruction: str
    start_sec: float
    end_sec: float
    analyze: bool
    runtime_axis: Optional[int] = None
    runtime_sign: int = 1

    samples: int = 0
    integrated_duration_sec: float = 0.0
    fallback_weight_samples: int = 0
    signed_integral: List[float] = field(
        default_factory=lambda: [0.0, 0.0, 0.0]
    )
    absolute_integral: List[float] = field(
        default_factory=lambda: [0.0, 0.0, 0.0]
    )
    signed_activity: List[float] = field(
        default_factory=lambda: [0.0, 0.0, 0.0]
    )
    absolute_activity: List[float] = field(
        default_factory=lambda: [0.0, 0.0, 0.0]
    )
    peak_abs: List[float] = field(
        default_factory=lambda: [0.0, 0.0, 0.0]
    )


def make_phases() -> List[PhaseStats]:
    return [
        PhaseStats(
            "still_start",
            "Hold the selected device still in a neutral pose.",
            0.0,
            4.0,
            False,
        ),
        PhaseStats(
            "roll_right",
            (
                "ROLL RIGHT: point the device forward with its top face up, then "
                "smoothly tilt its top toward your right around the forward axis."
            ),
            4.0,
            10.0,
            True,
            2,
            -1,
        ),
        PhaseStats(
            "neutral_1",
            "Return to the neutral pose and hold still.",
            10.0,
            12.0,
            False,
        ),
        PhaseStats(
            "pitch_up",
            "PITCH UP: smoothly raise the front/nose of the device upward.",
            12.0,
            18.0,
            True,
            0,
            +1,
        ),
        PhaseStats(
            "neutral_2",
            "Return to the neutral pose and hold still.",
            18.0,
            20.0,
            False,
        ),
        PhaseStats(
            "yaw_right",
            (
                "YAW RIGHT: keep the device level and smoothly turn its "
                "front/nose toward your right."
            ),
            20.0,
            26.0,
            True,
            1,
            -1,
        ),
        PhaseStats(
            "still_end",
            "Hold still while the recording finishes.",
            26.0,
            30.0,
            False,
        ),
    ]


def read_registry(path: Path, stream: str) -> RegistryInfo:
    try:
        with path.open("r", encoding="utf-8") as file:
            data = json.load(file)
    except FileNotFoundError as exc:
        raise RuntimeError(
            f"Registry not found: {path}. Start the controller-input publisher first."
        ) from exc
    except json.JSONDecodeError as exc:
        raise RuntimeError(f"Invalid JSON registry: {path}: {exc}") from exc

    streams = data.get("streams") or {}
    if stream not in streams:
        available = ", ".join(sorted(streams))
        raise RuntimeError(
            f"Stream {stream!r} was not found in {path}. "
            f"Available streams: {available or '<none>'}"
        )

    entry = streams[stream]
    return RegistryInfo(
        shm_name=str(entry.get("shm_name", stream)),
        header_size=int(entry.get("header_size", 4096)),
        slot_count=int(entry.get("slot_count", 32)),
        slot_stride=int(entry.get("slot_stride", 128 + V3_FRAME_SIZE)),
        slot_header_size=int(entry.get("slot_header_size", 128)),
        payload_size=int(entry.get("payload_size", V3_FRAME_SIZE)),
    )


def shm_path_for_name(name: str) -> Path:
    return Path("/dev/shm") / name.lstrip("/")


def read_latest_sequence(mm: mmap.mmap) -> int:
    latest = 0

    if len(mm) >= RING_HEADER_STRUCT.size:
        try:
            (
                _magic,
                _version,
                _header_size,
                _slot_count,
                _slot_stride,
                _slot_header_size,
                _payload_size,
                _reserved,
                latest,
            ) = RING_HEADER_STRUCT.unpack_from(mm, 0)
        except (ValueError, struct.error):
            latest = 0

    # Compatibility with the older publisher/reader header offset.
    if len(mm) >= 48:
        legacy_latest = struct.unpack_from("<Q", mm, 40)[0]
        if (
            legacy_latest
            and legacy_latest < (1 << 32)
            and (latest == 0 or latest > (1 << 32))
        ):
            latest = legacy_latest

    return latest


def read_latest_payload(
    mm: mmap.mmap,
    info: RegistryInfo,
    expected_sequence: Optional[int] = None,
) -> Optional[bytes]:
    latest = (
        expected_sequence
        if expected_sequence is not None
        else read_latest_sequence(mm)
    )
    if latest == 0:
        return None

    slot_index = (latest - 1) % info.slot_count
    slot_offset = info.header_size + slot_index * info.slot_stride

    required_size = slot_offset + info.slot_header_size + info.payload_size
    if required_size > len(mm):
        return None

    (
        seq_begin_1,
        seq_end_1,
        _timestamp_ns,
        _source_timestamp_ns,
        payload_size,
        _flags,
    ) = SLOT_HEADER_STRUCT.unpack_from(mm, slot_offset)

    if seq_begin_1 != seq_end_1 or (seq_begin_1 & 1):
        return None
    if payload_size < 8:
        return None

    payload_offset = slot_offset + info.slot_header_size
    payload_length = min(payload_size, info.payload_size)
    payload = bytes(mm[payload_offset : payload_offset + payload_length])

    seq_begin_2, seq_end_2, *_ = SLOT_HEADER_STRUCT.unpack_from(mm, slot_offset)
    if seq_begin_1 != seq_begin_2 or seq_end_1 != seq_end_2:
        return None

    return payload


def parse_imu(payload: bytes, side: str) -> ImuState:
    if len(payload) < V3_FRAME_SIZE:
        raise RuntimeError(
            f"Invalid controller_input payload: got {len(payload)} bytes, "
            f"expected at least {V3_FRAME_SIZE}"
        )

    version, size_bytes = struct.unpack_from("<II", payload, 0)
    if version != 3 or size_bytes != V3_FRAME_SIZE:
        raise RuntimeError(
            f"Unsupported controller_input ABI: version={version}, "
            f"size={size_bytes}; expected version=3, size={V3_FRAME_SIZE}"
        )

    side_base = FRAME_HEADER_SIZE + (0 if side == "left" else V3_SIDE_SIZE)
    imu_base = side_base + V3_IMU_OFFSET

    status, _capabilities, data_flags, sample_count = struct.unpack_from(
        "<IIII", payload, imu_base
    )
    sequence = struct.unpack_from("<Q", payload, imu_base + 16)[0]
    orientation = struct.unpack_from("<4f", payload, imu_base + 48)

    samples: List[ImuSample] = []
    for index in range(min(sample_count, 4)):
        sample_base = imu_base + IMU_SAMPLES_OFFSET + index * IMU_SAMPLE_SIZE
        timestamp_ns, device_ticks = struct.unpack_from(
            "<QQ", payload, sample_base
        )
        gyro = struct.unpack_from("<3f", payload, sample_base + 16)
        accel = struct.unpack_from("<3f", payload, sample_base + 28)
        flags = struct.unpack_from("<I", payload, sample_base + 40)[0]

        if all(math.isfinite(value) for value in (*gyro, *accel)):
            samples.append(
                ImuSample(
                    timestamp_ns=timestamp_ns,
                    device_timestamp_ticks=device_ticks,
                    gyro=gyro,
                    accel=accel,
                    flags=flags,
                )
            )

    return ImuState(
        status=status,
        data_flags=data_flags,
        sequence=sequence,
        orientation_xyzw=orientation,
        samples=samples,
    )


def phase_at(phases: Sequence[PhaseStats], elapsed: float) -> PhaseStats:
    for phase in phases:
        if phase.start_sec <= elapsed < phase.end_sec:
            return phase
    return phases[-1]


def matmul(
    left: Sequence[Sequence[float]],
    right: Sequence[Sequence[float]],
) -> List[List[float]]:
    return [
        [
            sum(left[row][index] * right[index][column] for index in range(3))
            for column in range(3)
        ]
        for row in range(3)
    ]


def rotation_matrix(
    rx_deg: int,
    ry_deg: int,
    rz_deg: int,
) -> List[List[float]]:
    rx, ry, rz = (math.radians(value) for value in (rx_deg, ry_deg, rz_deg))

    cx, sx = math.cos(rx), math.sin(rx)
    cy, sy = math.cos(ry), math.sin(ry)
    cz, sz = math.cos(rz), math.sin(rz)

    rx_matrix = [
        [1.0, 0.0, 0.0],
        [0.0, cx, -sx],
        [0.0, sx, cx],
    ]
    ry_matrix = [
        [cy, 0.0, sy],
        [0.0, 1.0, 0.0],
        [-sy, 0.0, cy],
    ]
    rz_matrix = [
        [cz, -sz, 0.0],
        [sz, cz, 0.0],
        [0.0, 0.0, 1.0],
    ]

    # Matches XR Gate orientation_transform order: Rz * Ry * Rx.
    return matmul(rz_matrix, matmul(ry_matrix, rx_matrix))


def max_matrix_error(
    left: Sequence[Sequence[float]],
    right: Sequence[Sequence[float]],
) -> float:
    return max(
        abs(left[row][column] - right[row][column])
        for row in range(3)
        for column in range(3)
    )


def find_config_for_matrix(
    target: Sequence[Sequence[float]],
) -> Optional[Dict[str, object]]:
    angles = (-180, -90, 0, 90, 180)
    best: Optional[Tuple[float, Dict[str, object]]] = None

    for invert_x in (False, True):
        for invert_y in (False, True):
            for invert_z in (False, True):
                inversion = [
                    [-1.0 if invert_x else 1.0, 0.0, 0.0],
                    [0.0, -1.0 if invert_y else 1.0, 0.0],
                    [0.0, 0.0, -1.0 if invert_z else 1.0],
                ]

                for rx_deg in angles:
                    for ry_deg in angles:
                        for rz_deg in angles:
                            candidate = matmul(
                                rotation_matrix(rx_deg, ry_deg, rz_deg),
                                inversion,
                            )
                            if max_matrix_error(candidate, target) > 1e-5:
                                continue

                            inversion_count = sum(
                                (invert_x, invert_y, invert_z)
                            )
                            nonzero_angles = sum(
                                value != 0
                                for value in (rx_deg, ry_deg, rz_deg)
                            )
                            score = (
                                abs(rx_deg)
                                + abs(ry_deg)
                                + abs(rz_deg)
                                + inversion_count * 20
                                + nonzero_angles * 2
                            )

                            config: Dict[str, object] = {
                                "enabled": True,
                                "invert_x": invert_x,
                                "invert_y": invert_y,
                                "invert_z": invert_z,
                                "basis_rotation": {
                                    "rx_deg": float(rx_deg),
                                    "ry_deg": float(ry_deg),
                                    "rz_deg": float(rz_deg),
                                },
                            }

                            if best is None or score < best[0]:
                                best = (score, config)

    return best[1] if best is not None else None


def dominant_axis(stats: PhaseStats) -> Tuple[int, int, float, str]:
    if stats.integrated_duration_sec >= 0.25:
        absolute_values = stats.absolute_integral
        signed_values = stats.signed_integral
        source = "time_integral"
    else:
        absolute_values = stats.absolute_activity
        signed_values = stats.signed_activity
        source = "sample_activity"

    dominant = max(range(3), key=lambda axis: absolute_values[axis])
    total = sum(absolute_values)
    purity = absolute_values[dominant] / total if total > 1e-9 else 0.0
    sign = 1 if signed_values[dominant] >= 0.0 else -1

    return dominant, sign, purity, source


def build_recommendation(
    movement_phases: Sequence[PhaseStats],
) -> Tuple[
    Optional[List[List[float]]],
    Optional[Dict[str, object]],
    List[str],
]:
    notes: List[str] = []
    mapping: Dict[int, Tuple[int, int, int, float]] = {}

    for phase in movement_phases:
        if phase.runtime_axis is None:
            continue

        raw_axis, measured_sign, purity, source = dominant_axis(phase)
        mapping[phase.runtime_axis] = (
            raw_axis,
            measured_sign,
            phase.runtime_sign,
            purity,
        )

        if purity < 0.65:
            notes.append(
                f"{phase.name}: low axis purity ({purity * 100:.0f}%); "
                "the device may have rotated around multiple axes"
            )
        if source == "sample_activity":
            notes.append(
                f"{phase.name}: source timestamps were insufficient, so axis "
                "detection used unintegrated sample activity"
            )

    if set(mapping) != {0, 1, 2}:
        notes.append("The diagnostic did not obtain all three runtime axes")
        return None, None, notes

    raw_axes = [mapping[runtime_axis][0] for runtime_axis in range(3)]
    if len(set(raw_axes)) != 3:
        notes.append(
            "Multiple physical motions mapped to the same raw axis; "
            "the automatic transform is unreliable"
        )
        return None, None, notes

    target = [[0.0, 0.0, 0.0] for _ in range(3)]
    for runtime_axis in range(3):
        raw_axis, measured_sign, expected_sign, _purity = mapping[runtime_axis]

        # The performed physical motion produced measured_sign on the raw axis.
        # XR Gate expects expected_sign on the corresponding runtime axis.
        target[runtime_axis][raw_axis] = float(
            expected_sign * measured_sign
        )

    config = find_config_for_matrix(target)
    if config is None:
        notes.append(
            "The resulting matrix could not be represented by "
            "invert_x/y/z plus basis_rotation Rz*Ry*Rx"
        )

    return target, config, notes


def compact_vector(values: Sequence[float]) -> str:
    return "[" + ", ".join(f"{value:+.3f}" for value in values) + "]"


def sample_key(
    imu_sequence: int,
    sample_index: int,
    sample: ImuSample,
) -> Tuple[int, int, int, int]:
    if sample.timestamp_ns != 0 or sample.device_timestamp_ticks != 0:
        return (
            sample.timestamp_ns,
            sample.device_timestamp_ticks,
            0,
            0,
        )
    return (0, 0, imu_sequence, sample_index)


def resolve_side(args: argparse.Namespace, parser: argparse.ArgumentParser) -> str:
    side = args.side_option or args.side
    if side is None:
        parser.error("choose a side: left or right")
    if args.side is not None and args.side_option is not None:
        if args.side != args.side_option:
            parser.error(
                f"conflicting side values: {args.side!r} and "
                f"{args.side_option!r}"
            )
    return side


def main() -> int:
    parser = argparse.ArgumentParser(
        description=(
            "Universal 30-second XR Gate IMU-axis diagnostic for "
            "controller_input V3 SHM"
        )
    )
    parser.add_argument(
        "side",
        nargs="?",
        choices=("left", "right"),
        help="controller side to inspect",
    )
    parser.add_argument(
        "--side",
        dest="side_option",
        choices=("left", "right"),
        help="controller side to inspect; retained for script compatibility",
    )
    parser.add_argument(
        "--registry",
        default=os.environ.get(
            "CONTROLLER_INPUT_REGISTRY",
            "/tmp/tracking_streams.json",
        ),
        help="stream registry path",
    )
    parser.add_argument(
        "--stream",
        default=os.environ.get(
            "CONTROLLER_INPUT_STREAM",
            "controller_input",
        ),
        help="controller-input stream name",
    )
    parser.add_argument(
        "--output",
        default="",
        help=(
            "JSON report path; defaults to "
            "/tmp/imu_axis_diag_<side>.json"
        ),
    )
    parser.add_argument(
        "--poll-ms",
        type=float,
        default=2.0,
        help="SHM polling interval in milliseconds",
    )
    parser.add_argument(
        "--countdown",
        type=int,
        default=3,
        help="countdown before recording starts",
    )
    args = parser.parse_args()
    side = resolve_side(args, parser)

    if args.poll_ms <= 0.0:
        parser.error("--poll-ms must be greater than zero")
    if args.countdown < 0:
        parser.error("--countdown cannot be negative")

    registry_path = Path(args.registry).expanduser()
    info = read_registry(registry_path, args.stream)
    shm_path = shm_path_for_name(info.shm_name)

    if not shm_path.exists():
        raise RuntimeError(
            f"SHM file not found: {shm_path}. "
            "Start the controller-input publisher first."
        )

    phases = make_phases()

    print("=" * 78)
    print(f"Universal IMU axis diagnostic: side={side}")
    print(f"Registry: {registry_path}")
    print(f"Stream:   {args.stream}")
    print(f"SHM:      {shm_path}")
    print(f"Duration: {RECORDING_DURATION_SEC:.0f} seconds")
    print()
    print("IMPORTANT:")
    print(
        "  Disable any upstream orientation_transform for the selected side "
        "before this test."
    )
    print(
        "  Hold the device with its intended forward direction pointing "
        "forward and its intended top facing up."
    )
    print(
        "  Start each requested motion from neutral and rotate around only "
        "the requested axis."
    )
    print("=" * 78)

    if args.countdown > 0:
        print(f"Recording starts in {args.countdown} seconds...")
        for remaining in range(args.countdown, 0, -1):
            print(f"  {remaining}", flush=True)
            time.sleep(1.0)

    last_ring_sequence = 0
    last_imu_sequence = 0
    last_source_timestamp_ns: Optional[int] = None
    last_packet_wall_time: Optional[float] = None
    last_phase_name = ""
    last_status_print = 0.0

    total_unique_samples = 0
    active_frames = 0
    imu_frames = 0
    source_timestamp_dt_samples = 0
    wall_clock_dt_samples = 0
    invalid_dt_samples = 0

    seen_keys: Set[Tuple[int, int, int, int]] = set()
    seen_order: Deque[Tuple[int, int, int, int]] = deque()
    max_seen_keys = 4096

    with shm_path.open("r+b") as shm_file:
        mm = mmap.mmap(shm_file.fileno(), 0, access=mmap.ACCESS_READ)
        start = time.monotonic()

        while True:
            now = time.monotonic()
            elapsed = now - start
            if elapsed >= RECORDING_DURATION_SEC:
                break

            phase = phase_at(phases, elapsed)
            if phase.name != last_phase_name:
                print()
                print("\a" + "#" * 78)
                print(
                    f"[{phase.start_sec:02.0f}-{phase.end_sec:02.0f} s] "
                    f"{phase.instruction}"
                )
                print("#" * 78, flush=True)
                last_phase_name = phase.name

            latest = read_latest_sequence(mm)
            if latest and latest != last_ring_sequence:
                payload = read_latest_payload(mm, info, latest)
                last_ring_sequence = latest

                if payload is not None:
                    imu = parse_imu(payload, side)
                    imu_frames += 1

                    if imu.status == IMU_STATUS_ACTIVE and imu.samples:
                        active_frames += 1

                    if (
                        imu.sequence != 0
                        and imu.sequence != last_imu_sequence
                        and imu.samples
                    ):
                        packet_wall_dt: Optional[float] = None
                        if last_packet_wall_time is not None:
                            candidate = now - last_packet_wall_time
                            if 0.0 < candidate <= 0.2:
                                packet_wall_dt = candidate
                        last_packet_wall_time = now
                        last_imu_sequence = imu.sequence

                        new_samples: List[Tuple[int, ImuSample]] = []
                        for sample_index, sample in enumerate(imu.samples):
                            key = sample_key(
                                imu.sequence,
                                sample_index,
                                sample,
                            )
                            if key in seen_keys:
                                continue

                            seen_keys.add(key)
                            seen_order.append(key)
                            if len(seen_order) > max_seen_keys:
                                old_key = seen_order.popleft()
                                seen_keys.discard(old_key)

                            new_samples.append((sample_index, sample))

                        fallback_dt = (
                            packet_wall_dt / len(new_samples)
                            if packet_wall_dt is not None and new_samples
                            else None
                        )

                        for _sample_index, sample in new_samples:
                            total_unique_samples += 1

                            dt = 0.0
                            dt_source = "invalid"

                            if (
                                sample.timestamp_ns != 0
                                and last_source_timestamp_ns is not None
                                and sample.timestamp_ns
                                > last_source_timestamp_ns
                            ):
                                candidate = (
                                    sample.timestamp_ns
                                    - last_source_timestamp_ns
                                ) / 1e9
                                if 0.0 < candidate <= 0.1:
                                    dt = candidate
                                    dt_source = "source_timestamp"

                            if sample.timestamp_ns != 0:
                                if (
                                    last_source_timestamp_ns is None
                                    or sample.timestamp_ns
                                    > last_source_timestamp_ns
                                ):
                                    last_source_timestamp_ns = (
                                        sample.timestamp_ns
                                    )

                            if dt == 0.0 and fallback_dt is not None:
                                dt = fallback_dt
                                dt_source = "wall_clock"

                            if dt_source == "source_timestamp":
                                source_timestamp_dt_samples += 1
                            elif dt_source == "wall_clock":
                                wall_clock_dt_samples += 1
                            else:
                                invalid_dt_samples += 1

                            if phase.analyze:
                                phase.samples += 1

                                for axis in range(3):
                                    gyro = sample.gyro[axis]
                                    phase.signed_activity[axis] += gyro
                                    phase.absolute_activity[axis] += abs(gyro)
                                    phase.peak_abs[axis] = max(
                                        phase.peak_abs[axis],
                                        abs(gyro),
                                    )

                                if dt > 0.0:
                                    phase.integrated_duration_sec += dt
                                    for axis in range(3):
                                        gyro = sample.gyro[axis]
                                        phase.signed_integral[axis] += (
                                            gyro * dt
                                        )
                                        phase.absolute_integral[axis] += (
                                            abs(gyro) * dt
                                        )
                                else:
                                    phase.fallback_weight_samples += 1

                        if (
                            now - last_status_print >= 0.25
                            and imu.samples
                        ):
                            gyro = imu.samples[-1].gyro
                            remaining = max(
                                0.0,
                                phase.end_sec - elapsed,
                            )
                            print(
                                f"\r  {remaining:4.1f} s remaining | "
                                f"gyro X={gyro[0]:+6.2f} "
                                f"Y={gyro[1]:+6.2f} "
                                f"Z={gyro[2]:+6.2f} rad/s",
                                end="",
                                flush=True,
                            )
                            last_status_print = now

            time.sleep(max(0.0005, args.poll_ms / 1000.0))

    print("\n\a")

    movement_phases = [phase for phase in phases if phase.analyze]
    target_matrix, suggested_config, notes = build_recommendation(
        movement_phases
    )

    if total_unique_samples < 100:
        notes.append(
            "Too few unique IMU samples were received for a reliable result"
        )
    if active_frames == 0:
        notes.append(
            "No active IMU frames were observed for the selected side"
        )

    sample_rate_hz = total_unique_samples / RECORDING_DURATION_SEC
    active_frame_ratio = (
        active_frames / imu_frames if imu_frames > 0 else 0.0
    )

    report: Dict[str, object] = {
        "format": REPORT_FORMAT,
        "source": "controller_input_v3",
        "side": side,
        "registry": str(registry_path),
        "stream": args.stream,
        "shm": str(shm_path),
        "recording_duration_sec": RECORDING_DURATION_SEC,
        "total_unique_imu_samples": total_unique_samples,
        "estimated_average_sample_rate_hz": sample_rate_hz,
        "imu_frames": imu_frames,
        "active_frames": active_frames,
        "active_frame_ratio": active_frame_ratio,
        "dt_sources": {
            "source_timestamp_samples": source_timestamp_dt_samples,
            "wall_clock_fallback_samples": wall_clock_dt_samples,
            "invalid_dt_samples": invalid_dt_samples,
        },
        "target_convention": {
            "coordinate_system": "XR Gate runtime_local / OpenXR aim pose",
            "axes": {
                "x": "right",
                "y": "up",
                "z": "backward; forward is -Z",
            },
            "motions": {
                "pitch_up": "+X",
                "yaw_right": "-Y",
                "roll_right": "-Z",
            },
        },
        "phases": {},
        "mapping": {},
        "target_matrix_output_from_raw": target_matrix,
        "suggested_orientation_transform": suggested_config,
        "notes": notes,
    }

    print("=" * 78)
    print("RESULT - COPY THE COMPLETE BLOCK BELOW")
    print("=" * 78)
    print(REPORT_FORMAT)
    print(f"side={side}")
    print(f"total_unique_imu_samples={total_unique_samples}")
    print(f"estimated_average_sample_rate_hz={sample_rate_hz:.1f}")
    print(f"active_frame_ratio={active_frame_ratio:.3f}")
    print(
        "dt_sources="
        f"source:{source_timestamp_dt_samples},"
        f"wall:{wall_clock_dt_samples},"
        f"invalid:{invalid_dt_samples}"
    )

    runtime_axis_names = ("PITCH/X", "YAW/Y", "ROLL/Z")

    for phase in movement_phases:
        raw_axis, sign, purity, detection_source = dominant_axis(phase)
        sign_text = "+" if sign > 0 else "-"
        expected_sign_text = "+" if phase.runtime_sign > 0 else "-"

        report["phases"][phase.name] = {
            "samples": phase.samples,
            "integrated_duration_sec": phase.integrated_duration_sec,
            "fallback_weight_samples": phase.fallback_weight_samples,
            "signed_integral_rad": phase.signed_integral,
            "absolute_integral_rad": phase.absolute_integral,
            "peak_abs_rad_s": phase.peak_abs,
            "dominant_raw_axis": AXIS_NAMES[raw_axis],
            "dominant_sign": sign_text,
            "purity": purity,
            "detection_source": detection_source,
        }

        runtime_axis = phase.runtime_axis
        if runtime_axis is not None:
            report["mapping"][runtime_axis_names[runtime_axis]] = {
                "raw_axis": AXIS_NAMES[raw_axis],
                "measured_sign": sign_text,
                "expected_runtime_sign": expected_sign_text,
                "purity": purity,
            }

        print(
            f"{phase.name}: samples={phase.samples} "
            f"signed={compact_vector(phase.signed_integral)} "
            f"abs={compact_vector(phase.absolute_integral)} "
            f"peak={compact_vector(phase.peak_abs)} "
            f"=> raw {sign_text}{AXIS_NAMES[raw_axis]}, "
            f"runtime target "
            f"{expected_sign_text}{AXIS_NAMES[runtime_axis or 0]}, "
            f"purity={purity * 100:.1f}%, "
            f"source={detection_source}"
        )

    if target_matrix is not None:
        print("target_matrix_output_from_raw=")
        for row in target_matrix:
            print("  " + compact_vector(row))

    print("suggested_orientation_transform=")
    if suggested_config is not None:
        print(json.dumps(suggested_config, ensure_ascii=False, indent=2))
    else:
        print("null")

    if notes:
        print("notes=")
        for note in notes:
            print(f"  - {note}")
    else:
        print("notes=[]")

    output_path = (
        Path(args.output).expanduser()
        if args.output
        else Path(f"/tmp/imu_axis_diag_{side}.json")
    )
    output_path.parent.mkdir(parents=True, exist_ok=True)

    with output_path.open("w", encoding="utf-8") as file:
        json.dump(report, file, ensure_ascii=False, indent=2)
        file.write("\n")

    print(f"json_report={output_path}")
    print("=" * 78)

    if total_unique_samples < 100 or active_frames == 0:
        return 2

    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except KeyboardInterrupt:
        print("\nDiagnostic interrupted.", file=sys.stderr)
        raise SystemExit(130)
    except Exception as exc:
        print(f"[ERROR] {exc}", file=sys.stderr)
        raise SystemExit(1)
