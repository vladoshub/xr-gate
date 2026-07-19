#!/usr/bin/env python3
"""
Guided IMU mount calibration for capture_service_cpp SHM.

The script reads the normalized IMU_F32_LE stream (normally ``imu0``) directly
from the POSIX shared-memory ring published by capture_service_cpp. It derives
the rigid rotation from the source IMU frame to the verified XREAL Ultra IMU
layout used by XR Gate.

Reference convention (current working XREAL Ultra layout):
  +X = right
  +Y = up
  +Z = backward (forward is -Z)

Expected positive physical motions:
  pitch up   -> +X angular velocity
  yaw right  -> -Y angular velocity
  roll right -> -Z angular velocity

Before running this tool, remove/comment ``imu.transform`` from the tested
capture_service_cpp profile and restart capture_service_cpp. Otherwise the tool
will calibrate the already transformed stream and the recommendation will be
wrong.

The default 30-second guided recording contains three one-way motions. On a
successful calibration the script prints one ready-to-paste YAML block:

  * ``axes: [...]`` for a mounting close to a signed 90-degree axis mapping;
  * ``quaternion_xyzw: [...]`` for an arbitrary rigid mounting angle;
  * no transform when the stream already matches the XREAL Ultra layout.

Only one of ``axes`` and ``quaternion_xyzw`` may be present in the config.

Examples:
  python3 calibrate_imu_axes.py
  python3 calibrate_imu_axes.py --registry /tmp/capture_service_streams.json
  python3 calibrate_imu_axes.py --stream imu0 --output /tmp/imu_mount.json
  python3 calibrate_imu_axes.py --self-test
"""

from __future__ import annotations

import argparse
import json
import math
import mmap
import os
import statistics
import struct
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Dict, Iterable, List, Optional, Sequence, Tuple


# capture_service_cpp POSIX SHM ABI:
# GlobalHeader: <8sIIIIIIQQ, exactly 48 bytes. latest_seq is at offset 40.
# SlotHeaderWire occupies 88 bytes and is padded to registry.slot_header_size
# (normally 128 bytes).
GLOBAL_HEADER_STRUCT = struct.Struct("<8s6I2Q")
SLOT_HEADER_STRUCT = struct.Struct("<QQqQ6I32s")
IMU_PAYLOAD_STRUCT = struct.Struct("<6f")

SHM_MAGIC = b"CAPSHM1\0"
SHM_VERSION = 1
FORMAT_IMU_F32_LE = 101
EXPECTED_IMU_PAYLOAD_SIZE = 24
REPORT_FORMAT = "CAPTURE_IMU_MOUNT_CALIBRATION_V1"
DEFAULT_DURATION_SEC = 30.0
AXIS_NAMES = ("x", "y", "z")

Vector3 = Tuple[float, float, float]
Matrix3 = List[List[float]]
Quaternion = Tuple[float, float, float, float]


@dataclass(frozen=True)
class RegistryInfo:
    registry_path: Path
    stream_id: str
    shm_name: str
    header_size: int
    slot_count: int
    slot_stride: int
    slot_header_size: int
    payload_size: int
    format_code: int
    format_name: str
    frame_id: str
    config_path: str
    profile: str
    namespace: str


@dataclass(frozen=True)
class ImuSample:
    sequence: int
    timestamp_ns: int
    monotonic_ns: int
    wall_elapsed_sec: float
    gyro: Vector3
    accel: Vector3


@dataclass(frozen=True)
class PhaseDefinition:
    name: str
    instruction: str
    start_sec: float
    end_sec: float
    analyze: bool


@dataclass
class MotionStats:
    name: str
    sample_count: int = 0
    active_sample_count: int = 0
    integrated_duration_sec: float = 0.0
    signed_integral_rad: List[float] = field(
        default_factory=lambda: [0.0, 0.0, 0.0]
    )
    absolute_angle_rad: float = 0.0
    peak_rad_s: float = 0.0
    coherence: float = 0.0
    direction_raw: Optional[Vector3] = None


@dataclass(frozen=True)
class Recommendation:
    success: bool
    mode: str
    rotation_matrix: Optional[Matrix3]
    axes: Optional[Tuple[str, str, str]]
    quaternion_xyzw: Optional[Quaternion]
    axes_residual_deg: Optional[float]
    identity_residual_deg: Optional[float]
    notes: Tuple[str, ...]


def make_phases(scale: float = 1.0) -> List[PhaseDefinition]:
    def t(value: float) -> float:
        return value * scale

    return [
        PhaseDefinition(
            "still_start",
            "Hold the device completely still in its intended neutral pose.",
            t(0.0),
            t(4.0),
            False,
        ),
        PhaseDefinition(
            "roll_right",
            (
                "ROLL RIGHT: point the device forward with its top facing up, "
                "then smoothly tilt the top toward your right and HOLD there."
            ),
            t(4.0),
            t(10.0),
            True,
        ),
        PhaseDefinition(
            "neutral_1",
            "Return to neutral and hold still.",
            t(10.0),
            t(12.0),
            False,
        ),
        PhaseDefinition(
            "pitch_up",
            "PITCH UP: smoothly raise the front/nose upward and HOLD there.",
            t(12.0),
            t(18.0),
            True,
        ),
        PhaseDefinition(
            "neutral_2",
            "Return to neutral and hold still.",
            t(18.0),
            t(20.0),
            False,
        ),
        PhaseDefinition(
            "yaw_right",
            (
                "YAW RIGHT: keep the device level, smoothly turn its front/nose "
                "toward your right and HOLD there."
            ),
            t(20.0),
            t(26.0),
            True,
        ),
        PhaseDefinition(
            "still_end",
            "Hold the device still while the recording finishes.",
            t(26.0),
            t(30.0),
            False,
        ),
    ]


def phase_at(phases: Sequence[PhaseDefinition], elapsed: float) -> PhaseDefinition:
    for phase in phases:
        if phase.start_sec <= elapsed < phase.end_sec:
            return phase
    return phases[-1]


def read_registry(path: Path, stream_id: str) -> RegistryInfo:
    try:
        with path.open("r", encoding="utf-8") as file:
            data = json.load(file)
    except FileNotFoundError as exc:
        raise RuntimeError(
            f"Registry not found: {path}. Start capture_service_cpp with SHM publishing first."
        ) from exc
    except json.JSONDecodeError as exc:
        raise RuntimeError(f"Invalid JSON registry {path}: {exc}") from exc

    streams = data.get("streams")
    if not isinstance(streams, dict):
        raise RuntimeError(f"Registry {path} has no object field 'streams'")
    if stream_id not in streams:
        available = ", ".join(sorted(streams)) or "<none>"
        raise RuntimeError(
            f"Stream {stream_id!r} not found in {path}. Available streams: {available}"
        )

    entry = streams[stream_id]
    if not isinstance(entry, dict):
        raise RuntimeError(f"Registry entry for {stream_id!r} is not an object")

    shm_name = str(entry.get("shm_name", ""))
    slot_count = int(entry.get("slot_count", 0))
    payload_size = int(entry.get("payload_size", 0))
    slot_header_size = int(entry.get("slot_header_size", 128))
    slot_stride = int(
        entry.get("slot_stride", slot_header_size + payload_size)
    )
    header_size = int(entry.get("header_size", 4096))
    format_code = int(entry.get("format_code", 0))
    format_name = str(entry.get("format_name", ""))

    if not shm_name:
        raise RuntimeError(f"Registry entry {stream_id!r} has no shm_name")
    if slot_count <= 0 or payload_size <= 0:
        raise RuntimeError(
            f"Invalid SHM dimensions for {stream_id!r}: "
            f"slot_count={slot_count}, payload_size={payload_size}"
        )
    if header_size < GLOBAL_HEADER_STRUCT.size:
        raise RuntimeError(f"Invalid header_size={header_size}")
    if slot_header_size < SLOT_HEADER_STRUCT.size:
        raise RuntimeError(f"Invalid slot_header_size={slot_header_size}")
    if slot_stride < slot_header_size + payload_size:
        raise RuntimeError(f"Invalid slot_stride={slot_stride}")
    if format_code != FORMAT_IMU_F32_LE:
        raise RuntimeError(
            f"Stream {stream_id!r} is not IMU_F32_LE: "
            f"format_code={format_code}, format_name={format_name!r}"
        )
    if payload_size < EXPECTED_IMU_PAYLOAD_SIZE:
        raise RuntimeError(
            f"IMU stream payload_size={payload_size}, expected at least 24"
        )

    return RegistryInfo(
        registry_path=path,
        stream_id=stream_id,
        shm_name=shm_name,
        header_size=header_size,
        slot_count=slot_count,
        slot_stride=slot_stride,
        slot_header_size=slot_header_size,
        payload_size=payload_size,
        format_code=format_code,
        format_name=format_name,
        frame_id=str(entry.get("frame_id", stream_id)),
        config_path=str(data.get("config_path", "")),
        profile=str(data.get("profile", "")),
        namespace=str(data.get("namespace", "")),
    )


def shm_path_for_name(name: str) -> Path:
    return Path("/dev/shm") / name.lstrip("/")


class CaptureImuShmReader:
    def __init__(self, info: RegistryInfo):
        self.info = info
        self.path = shm_path_for_name(info.shm_name)
        if not self.path.exists():
            raise RuntimeError(
                f"SHM file not found: {self.path}. Start capture_service_cpp first."
            )

        expected_size = (
            info.header_size + info.slot_count * info.slot_stride
        )
        actual_size = self.path.stat().st_size
        if actual_size < expected_size:
            raise RuntimeError(
                f"SHM file is too small: {actual_size} bytes, expected at least "
                f"{expected_size}"
            )

        self._file = self.path.open("rb")
        self._mm = mmap.mmap(self._file.fileno(), 0, access=mmap.ACCESS_READ)
        self._validate_global_header()

    def close(self) -> None:
        self._mm.close()
        self._file.close()

    def __enter__(self) -> "CaptureImuShmReader":
        return self

    def __exit__(self, exc_type, exc, traceback) -> None:
        self.close()

    def _validate_global_header(self) -> None:
        if len(self._mm) < GLOBAL_HEADER_STRUCT.size:
            raise RuntimeError("SHM is smaller than GlobalHeader")
        (
            magic,
            version,
            _kind,
            slot_count,
            slot_header_size,
            payload_size,
            header_size,
            _created_ns,
            _latest_seq,
        ) = GLOBAL_HEADER_STRUCT.unpack_from(self._mm, 0)

        if magic != SHM_MAGIC:
            raise RuntimeError(
                f"Invalid SHM magic {magic!r}; expected {SHM_MAGIC!r}"
            )
        if version != SHM_VERSION:
            raise RuntimeError(
                f"Unsupported SHM version {version}; expected {SHM_VERSION}"
            )
        expected = self.info
        mismatches = []
        if slot_count != expected.slot_count:
            mismatches.append(
                f"slot_count registry={expected.slot_count} shm={slot_count}"
            )
        if slot_header_size != expected.slot_header_size:
            mismatches.append(
                "slot_header_size "
                f"registry={expected.slot_header_size} shm={slot_header_size}"
            )
        if payload_size != expected.payload_size:
            mismatches.append(
                f"payload_size registry={expected.payload_size} shm={payload_size}"
            )
        if header_size != expected.header_size:
            mismatches.append(
                f"header_size registry={expected.header_size} shm={header_size}"
            )
        if mismatches:
            raise RuntimeError(
                "Registry/SHM layout mismatch: " + "; ".join(mismatches)
            )

    def latest_sequence(self) -> int:
        # latest_seq is the last uint64 in the packed 48-byte header.
        return struct.unpack_from("<Q", self._mm, 40)[0]

    def read_sequence(self, sequence: int, wall_elapsed_sec: float) -> Optional[ImuSample]:
        if sequence <= 0:
            return None

        latest = self.latest_sequence()
        if latest == 0 or sequence > latest:
            return None
        if latest - sequence >= self.info.slot_count:
            return None

        slot_index = (sequence - 1) % self.info.slot_count
        slot_offset = (
            self.info.header_size + slot_index * self.info.slot_stride
        )
        payload_offset = slot_offset + self.info.slot_header_size
        required = payload_offset + self.info.payload_size
        if required > len(self._mm):
            return None

        header_1 = SLOT_HEADER_STRUCT.unpack_from(self._mm, slot_offset)
        (
            seq_begin_1,
            seq_end_1,
            timestamp_ns,
            monotonic_ns,
            payload_len,
            _width,
            _height,
            format_code,
            _flags,
            _reserved,
            _frame_id,
        ) = header_1

        if seq_begin_1 != seq_end_1 or seq_begin_1 == 0 or (seq_begin_1 & 1):
            return None
        actual_sequence = seq_end_1 // 2
        if actual_sequence != sequence:
            return None
        if format_code != FORMAT_IMU_F32_LE:
            return None
        if payload_len < EXPECTED_IMU_PAYLOAD_SIZE:
            return None
        if payload_len > self.info.payload_size:
            return None

        payload = bytes(
            self._mm[payload_offset : payload_offset + EXPECTED_IMU_PAYLOAD_SIZE]
        )
        header_2 = SLOT_HEADER_STRUCT.unpack_from(self._mm, slot_offset)
        if header_2[0] != seq_begin_1 or header_2[1] != seq_end_1:
            return None

        values = IMU_PAYLOAD_STRUCT.unpack(payload)
        if not all(math.isfinite(value) for value in values):
            return None

        return ImuSample(
            sequence=actual_sequence,
            timestamp_ns=int(timestamp_ns),
            monotonic_ns=int(monotonic_ns),
            wall_elapsed_sec=wall_elapsed_sec,
            gyro=(values[0], values[1], values[2]),
            accel=(values[3], values[4], values[5]),
        )


# --------------------------- Vector/matrix helpers ---------------------------


def v_add(left: Vector3, right: Vector3) -> Vector3:
    return (
        left[0] + right[0],
        left[1] + right[1],
        left[2] + right[2],
    )


def v_sub(left: Vector3, right: Vector3) -> Vector3:
    return (
        left[0] - right[0],
        left[1] - right[1],
        left[2] - right[2],
    )


def v_scale(value: Vector3, scale: float) -> Vector3:
    return (value[0] * scale, value[1] * scale, value[2] * scale)


def v_dot(left: Vector3, right: Vector3) -> float:
    return left[0] * right[0] + left[1] * right[1] + left[2] * right[2]


def v_norm(value: Vector3) -> float:
    return math.sqrt(v_dot(value, value))


def v_normalize(value: Vector3) -> Vector3:
    norm = v_norm(value)
    if not math.isfinite(norm) or norm < 1e-12:
        raise ValueError("cannot normalize a zero/non-finite vector")
    return v_scale(value, 1.0 / norm)


def v_cross(left: Vector3, right: Vector3) -> Vector3:
    return (
        left[1] * right[2] - left[2] * right[1],
        left[2] * right[0] - left[0] * right[2],
        left[0] * right[1] - left[1] * right[0],
    )


def clamp(value: float, low: float, high: float) -> float:
    return max(low, min(high, value))


def angle_between_deg(left: Vector3, right: Vector3) -> float:
    denominator = v_norm(left) * v_norm(right)
    if denominator < 1e-12:
        return 180.0
    cosine = clamp(v_dot(left, right) / denominator, -1.0, 1.0)
    return math.degrees(math.acos(cosine))


def matrix_identity() -> Matrix3:
    return [[1.0, 0.0, 0.0], [0.0, 1.0, 0.0], [0.0, 0.0, 1.0]]


def matrix_transpose(matrix: Sequence[Sequence[float]]) -> Matrix3:
    return [[float(matrix[column][row]) for column in range(3)] for row in range(3)]


def matrix_add(left: Sequence[Sequence[float]], right: Sequence[Sequence[float]]) -> Matrix3:
    return [
        [float(left[row][column] + right[row][column]) for column in range(3)]
        for row in range(3)
    ]


def matrix_scale(matrix: Sequence[Sequence[float]], scale: float) -> Matrix3:
    return [
        [float(matrix[row][column] * scale) for column in range(3)]
        for row in range(3)
    ]


def matrix_multiply(left: Sequence[Sequence[float]], right: Sequence[Sequence[float]]) -> Matrix3:
    return [
        [
            sum(left[row][index] * right[index][column] for index in range(3))
            for column in range(3)
        ]
        for row in range(3)
    ]


def matrix_determinant(matrix: Sequence[Sequence[float]]) -> float:
    a, b, c = matrix[0]
    d, e, f = matrix[1]
    g, h, i = matrix[2]
    return (
        a * (e * i - f * h)
        - b * (d * i - f * g)
        + c * (d * h - e * g)
    )


def matrix_inverse(matrix: Sequence[Sequence[float]]) -> Matrix3:
    a, b, c = matrix[0]
    d, e, f = matrix[1]
    g, h, i = matrix[2]
    determinant = matrix_determinant(matrix)
    if not math.isfinite(determinant) or abs(determinant) < 1e-10:
        raise ValueError("matrix is singular")
    inverse_determinant = 1.0 / determinant
    return [
        [
            (e * i - f * h) * inverse_determinant,
            (c * h - b * i) * inverse_determinant,
            (b * f - c * e) * inverse_determinant,
        ],
        [
            (f * g - d * i) * inverse_determinant,
            (a * i - c * g) * inverse_determinant,
            (c * d - a * f) * inverse_determinant,
        ],
        [
            (d * h - e * g) * inverse_determinant,
            (b * g - a * h) * inverse_determinant,
            (a * e - b * d) * inverse_determinant,
        ],
    ]


def matrix_max_error(left: Sequence[Sequence[float]], right: Sequence[Sequence[float]]) -> float:
    return max(
        abs(left[row][column] - right[row][column])
        for row in range(3)
        for column in range(3)
    )


def closest_rotation(matrix: Sequence[Sequence[float]]) -> Matrix3:
    """Return the closest proper rotation using Newton polar iteration."""
    result = [[float(value) for value in row] for row in matrix]
    if matrix_determinant(result) <= 0.0:
        raise ValueError("measured basis is left-handed/reflected")

    for _ in range(20):
        inverse_transpose = matrix_transpose(matrix_inverse(result))
        updated = matrix_scale(matrix_add(result, inverse_transpose), 0.5)
        error = matrix_max_error(updated, result)
        result = updated
        if error < 1e-12:
            break

    determinant = matrix_determinant(result)
    if determinant < 0.999 or determinant > 1.001:
        raise ValueError(
            f"failed to obtain a proper rotation; determinant={determinant:.6f}"
        )
    return result


def rotation_difference_deg(left: Sequence[Sequence[float]], right: Sequence[Sequence[float]]) -> float:
    relative = matrix_multiply(left, matrix_transpose(right))
    cosine = clamp(
        (relative[0][0] + relative[1][1] + relative[2][2] - 1.0) / 2.0,
        -1.0,
        1.0,
    )
    return math.degrees(math.acos(cosine))


def matrix_to_quaternion_xyzw(matrix: Sequence[Sequence[float]]) -> Quaternion:
    m00, m01, m02 = matrix[0]
    m10, m11, m12 = matrix[1]
    m20, m21, m22 = matrix[2]
    trace = m00 + m11 + m22

    if trace > 0.0:
        s = math.sqrt(trace + 1.0) * 2.0
        w = 0.25 * s
        x = (m21 - m12) / s
        y = (m02 - m20) / s
        z = (m10 - m01) / s
    elif m00 > m11 and m00 > m22:
        s = math.sqrt(max(0.0, 1.0 + m00 - m11 - m22)) * 2.0
        w = (m21 - m12) / s
        x = 0.25 * s
        y = (m01 + m10) / s
        z = (m02 + m20) / s
    elif m11 > m22:
        s = math.sqrt(max(0.0, 1.0 + m11 - m00 - m22)) * 2.0
        w = (m02 - m20) / s
        x = (m01 + m10) / s
        y = 0.25 * s
        z = (m12 + m21) / s
    else:
        s = math.sqrt(max(0.0, 1.0 + m22 - m00 - m11)) * 2.0
        w = (m10 - m01) / s
        x = (m02 + m20) / s
        y = (m12 + m21) / s
        z = 0.25 * s

    norm = math.sqrt(x * x + y * y + z * z + w * w)
    if norm < 1e-12:
        raise ValueError("rotation produced a zero quaternion")
    x, y, z, w = x / norm, y / norm, z / norm, w / norm
    # q and -q are equivalent; a non-negative w makes reports stable.
    if w < 0.0:
        x, y, z, w = -x, -y, -z, -w
    return (x, y, z, w)


def quaternion_xyzw_to_matrix(quaternion: Quaternion) -> Matrix3:
    x, y, z, w = quaternion
    norm = math.sqrt(x * x + y * y + z * z + w * w)
    if norm < 1e-12:
        raise ValueError("zero quaternion")
    x, y, z, w = x / norm, y / norm, z / norm, w / norm
    return [
        [
            1.0 - 2.0 * (y * y + z * z),
            2.0 * (x * y - z * w),
            2.0 * (x * z + y * w),
        ],
        [
            2.0 * (x * y + z * w),
            1.0 - 2.0 * (x * x + z * z),
            2.0 * (y * z - x * w),
        ],
        [
            2.0 * (x * z - y * w),
            2.0 * (y * z + x * w),
            1.0 - 2.0 * (x * x + y * y),
        ],
    ]


def signed_permutation_candidate(rotation: Sequence[Sequence[float]]) -> Tuple[Tuple[str, str, str], Matrix3]:
    used_columns = set()
    tokens: List[str] = []
    candidate = [[0.0, 0.0, 0.0] for _ in range(3)]

    for row in range(3):
        column = max(range(3), key=lambda index: abs(rotation[row][index]))
        if column in used_columns:
            raise ValueError("rotation does not have a unique dominant source axis per output axis")
        used_columns.add(column)
        sign = -1.0 if rotation[row][column] < 0.0 else 1.0
        candidate[row][column] = sign
        tokens.append(("-" if sign < 0.0 else "") + AXIS_NAMES[column])

    if matrix_determinant(candidate) < 0.5:
        raise ValueError("closest signed-axis mapping is a reflection")
    return (tokens[0], tokens[1], tokens[2]), candidate


def compact_vector(values: Sequence[float], digits: int = 4) -> str:
    return "[" + ", ".join(f"{value:+.{digits}f}" for value in values) + "]"


def compact_matrix(matrix: Sequence[Sequence[float]], digits: int = 6) -> List[List[float]]:
    return [
        [round(float(matrix[row][column]), digits) for column in range(3)]
        for row in range(3)
    ]


def median_vector(values: Sequence[Vector3]) -> Vector3:
    if not values:
        return (0.0, 0.0, 0.0)
    return (
        float(statistics.median(value[0] for value in values)),
        float(statistics.median(value[1] for value in values)),
        float(statistics.median(value[2] for value in values)),
    )


def robust_noise_sigma(values: Sequence[Vector3], center: Vector3) -> Vector3:
    if not values:
        return (0.0, 0.0, 0.0)
    sigmas = []
    for axis in range(3):
        deviations = [abs(value[axis] - center[axis]) for value in values]
        mad = float(statistics.median(deviations))
        sigmas.append(1.4826 * mad)
    return (sigmas[0], sigmas[1], sigmas[2])


def median_positive_timestamp_dt(samples: Sequence[ImuSample]) -> Optional[float]:
    values: List[float] = []
    for previous, current in zip(samples, samples[1:]):
        if current.timestamp_ns > previous.timestamp_ns:
            dt = (current.timestamp_ns - previous.timestamp_ns) / 1e9
            if 0.00005 <= dt <= 0.05:
                values.append(dt)
    if not values:
        return None
    return float(statistics.median(values))


def phase_samples(samples: Sequence[ImuSample], phase: PhaseDefinition) -> List[ImuSample]:
    return [
        sample
        for sample in samples
        if phase.start_sec <= sample.wall_elapsed_sec < phase.end_sec
    ]


def analyze_motion(
    name: str,
    samples: Sequence[ImuSample],
    bias: Vector3,
    active_threshold_rad_s: float,
    fallback_dt_sec: float,
) -> MotionStats:
    stats = MotionStats(name=name, sample_count=len(samples))
    previous: Optional[ImuSample] = None

    for sample in samples:
        corrected = v_sub(sample.gyro, bias)
        magnitude = v_norm(corrected)
        stats.peak_rad_s = max(stats.peak_rad_s, magnitude)

        dt = fallback_dt_sec
        if previous is not None and sample.timestamp_ns > previous.timestamp_ns:
            candidate = (sample.timestamp_ns - previous.timestamp_ns) / 1e9
            if 0.00005 <= candidate <= 0.05:
                dt = candidate
        previous = sample

        if magnitude < active_threshold_rad_s:
            continue

        stats.active_sample_count += 1
        stats.integrated_duration_sec += dt
        stats.absolute_angle_rad += magnitude * dt
        for axis in range(3):
            stats.signed_integral_rad[axis] += corrected[axis] * dt

    signed_vector: Vector3 = (
        stats.signed_integral_rad[0],
        stats.signed_integral_rad[1],
        stats.signed_integral_rad[2],
    )
    signed_norm = v_norm(signed_vector)
    if stats.absolute_angle_rad > 1e-12:
        stats.coherence = signed_norm / stats.absolute_angle_rad
    if signed_norm > 1e-12:
        stats.direction_raw = v_scale(signed_vector, 1.0 / signed_norm)
    return stats


def build_recommendation(
    roll: MotionStats,
    pitch: MotionStats,
    yaw: MotionStats,
    axes_max_error_deg: float,
) -> Recommendation:
    notes: List[str] = []
    motions = (roll, pitch, yaw)

    for motion in motions:
        if motion.direction_raw is None:
            notes.append(f"{motion.name}: no usable angular motion was detected")
        if motion.absolute_angle_rad < 0.25:
            notes.append(
                f"{motion.name}: integrated motion is too small "
                f"({math.degrees(motion.absolute_angle_rad):.1f} deg; need roughly 15+ deg)"
            )
        if motion.coherence < 0.65:
            notes.append(
                f"{motion.name}: low one-way motion coherence "
                f"({motion.coherence * 100.0:.1f}%); repeat without wobbling or returning during the phase"
            )

    if any(motion.direction_raw is None for motion in motions):
        return Recommendation(False, "none", None, None, None, None, None, tuple(notes))
    if any(motion.absolute_angle_rad < 0.25 for motion in motions):
        return Recommendation(False, "none", None, None, None, None, None, tuple(notes))
    if any(motion.coherence < 0.65 for motion in motions):
        return Recommendation(False, "none", None, None, None, None, None, tuple(notes))

    # Each row is the raw-frame direction corresponding to a positive canonical
    # XREAL Ultra output axis. Per the guided motions:
    #   pitch up  = +X, yaw right = -Y, roll right = -Z.
    measured_rows: Matrix3 = [
        list(pitch.direction_raw),
        list(v_scale(yaw.direction_raw, -1.0)),
        list(v_scale(roll.direction_raw, -1.0)),
    ]

    pair_angles = {
        "xy": angle_between_deg(tuple(measured_rows[0]), tuple(measured_rows[1])),
        "xz": angle_between_deg(tuple(measured_rows[0]), tuple(measured_rows[2])),
        "yz": angle_between_deg(tuple(measured_rows[1]), tuple(measured_rows[2])),
    }
    for pair, angle in pair_angles.items():
        if abs(angle - 90.0) > 22.0:
            notes.append(
                f"measured {pair.upper()} motion axes are {angle:.1f} deg apart; expected about 90 deg"
            )

    determinant = matrix_determinant(measured_rows)
    if determinant <= 0.20:
        notes.append(
            "measured motions form a reflected or nearly singular basis; check motion directions"
        )

    if notes:
        return Recommendation(False, "none", None, None, None, None, None, tuple(notes))

    try:
        rotation = closest_rotation(measured_rows)
    except ValueError as exc:
        notes.append(str(exc))
        return Recommendation(False, "none", None, None, None, None, None, tuple(notes))

    residuals = [
        angle_between_deg(tuple(measured_rows[row]), tuple(rotation[row]))
        for row in range(3)
    ]
    if max(residuals) > 15.0:
        notes.append(
            "orthogonal-fit residual is too high: "
            + ", ".join(f"{value:.1f} deg" for value in residuals)
        )
        return Recommendation(False, "none", rotation, None, None, None, None, tuple(notes))

    quaternion = matrix_to_quaternion_xyzw(rotation)
    reconstructed = quaternion_xyzw_to_matrix(quaternion)
    if matrix_max_error(rotation, reconstructed) > 1e-6:
        notes.append("internal quaternion conversion check failed")
        return Recommendation(False, "none", rotation, None, None, None, None, tuple(notes))

    identity_error = rotation_difference_deg(rotation, matrix_identity())

    axes: Optional[Tuple[str, str, str]] = None
    axes_error: Optional[float] = None
    try:
        candidate_axes, candidate_matrix = signed_permutation_candidate(rotation)
        axes_error = rotation_difference_deg(rotation, candidate_matrix)
        if axes_error <= axes_max_error_deg:
            axes = candidate_axes
    except ValueError as exc:
        notes.append(f"signed-axis candidate unavailable: {exc}")

    if identity_error <= axes_max_error_deg:
        return Recommendation(
            True,
            "identity",
            rotation,
            ("x", "y", "z"),
            quaternion,
            axes_error,
            identity_error,
            tuple(notes),
        )
    if axes is not None:
        return Recommendation(
            True,
            "axes",
            rotation,
            axes,
            quaternion,
            axes_error,
            identity_error,
            tuple(notes),
        )
    return Recommendation(
        True,
        "quaternion",
        rotation,
        None,
        quaternion,
        axes_error,
        identity_error,
        tuple(notes),
    )


def yaml_for_recommendation(recommendation: Recommendation) -> str:
    if not recommendation.success:
        return "# Calibration failed; do not change imu.transform."
    if recommendation.mode == "identity":
        return (
            "# The stream already matches the XREAL Ultra axis layout.\n"
            "# Remove/omit the entire imu.transform section."
        )
    if recommendation.mode == "axes" and recommendation.axes is not None:
        return (
            "imu:\n"
            "  transform:\n"
            f"    axes: [{recommendation.axes[0]}, {recommendation.axes[1]}, {recommendation.axes[2]}]"
        )
    if recommendation.mode == "quaternion" and recommendation.quaternion_xyzw is not None:
        values = ", ".join(
            f"{value:.9f}" for value in recommendation.quaternion_xyzw
        )
        return (
            "imu:\n"
            "  transform:\n"
            f"    quaternion_xyzw: [{values}]"
        )
    return "# Internal error: recommendation is incomplete."


def possible_transform_warning(config_path: str) -> Optional[str]:
    if not config_path or config_path.startswith("<built-in:"):
        return None
    path = Path(config_path).expanduser()
    try:
        text = path.read_text(encoding="utf-8")
    except OSError:
        return None

    # This is intentionally only a warning, not a YAML parser. It detects a
    # transform key inside the indentation range of an imu: section.
    in_imu = False
    imu_indent = 0
    for raw_line in text.splitlines():
        line_without_comment = raw_line.split("#", 1)[0].rstrip()
        if not line_without_comment.strip():
            continue
        indent = len(line_without_comment) - len(line_without_comment.lstrip(" "))
        stripped = line_without_comment.strip()
        if stripped == "imu:":
            in_imu = True
            imu_indent = indent
            continue
        if in_imu and indent <= imu_indent:
            in_imu = False
        if in_imu and stripped == "transform:":
            return (
                f"The selected config appears to contain imu.transform: {path}. "
                "Remove/comment it and restart capture_service_cpp before calibration."
            )
    return None


def run_self_test() -> int:
    identity = matrix_identity()
    axes_matrix: Matrix3 = [
        [1.0, 0.0, 0.0],
        [0.0, 0.0, -1.0],
        [0.0, 1.0, 0.0],
    ]
    arbitrary_q: Quaternion = (0.120590477, -0.205991123, 0.171010072, 0.956962356)
    arbitrary_matrix = quaternion_xyzw_to_matrix(arbitrary_q)

    for name, matrix in (
        ("identity", identity),
        ("axes", axes_matrix),
        ("arbitrary", arbitrary_matrix),
    ):
        fitted = closest_rotation(matrix)
        if matrix_max_error(fitted, matrix) > 1e-8:
            raise AssertionError(f"{name}: polar fit mismatch")
        quaternion = matrix_to_quaternion_xyzw(fitted)
        roundtrip = quaternion_xyzw_to_matrix(quaternion)
        if matrix_max_error(roundtrip, matrix) > 1e-7:
            raise AssertionError(f"{name}: quaternion roundtrip mismatch")

    tokens, candidate = signed_permutation_candidate(axes_matrix)
    if tokens != ("x", "-z", "y"):
        raise AssertionError(f"unexpected axes tokens: {tokens}")
    if matrix_max_error(candidate, axes_matrix) > 1e-12:
        raise AssertionError("axes candidate matrix mismatch")

    # Synthetic guided motions for source->output R. The raw motion direction
    # for an output unit axis is the corresponding row of R.
    def synthetic_motion(name: str, direction: Vector3, expected_sign: float) -> MotionStats:
        vector = v_scale(direction, expected_sign * math.radians(55.0))
        return MotionStats(
            name=name,
            sample_count=100,
            active_sample_count=100,
            integrated_duration_sec=1.0,
            signed_integral_rad=list(vector),
            absolute_angle_rad=math.radians(55.0),
            peak_rad_s=1.0,
            coherence=1.0,
            direction_raw=v_normalize(vector),
        )

    for expected_mode, matrix in (
        ("identity", identity),
        ("axes", axes_matrix),
        ("quaternion", arbitrary_matrix),
    ):
        pitch = synthetic_motion("pitch_up", tuple(matrix[0]), +1.0)
        yaw = synthetic_motion("yaw_right", tuple(matrix[1]), -1.0)
        roll = synthetic_motion("roll_right", tuple(matrix[2]), -1.0)
        recommendation = build_recommendation(roll, pitch, yaw, 8.0)
        if not recommendation.success or recommendation.mode != expected_mode:
            raise AssertionError(
                f"{expected_mode}: got success={recommendation.success}, "
                f"mode={recommendation.mode}, notes={recommendation.notes}"
            )
        if recommendation.rotation_matrix is None:
            raise AssertionError(f"{expected_mode}: missing rotation matrix")
        if matrix_max_error(recommendation.rotation_matrix, matrix) > 1e-6:
            raise AssertionError(f"{expected_mode}: recovered matrix mismatch")

    print("Self-test: OK")
    return 0


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Calibrate capture_service_cpp IMU axes/mount rotation against the "
            "verified XREAL Ultra coordinate layout"
        )
    )
    parser.add_argument(
        "--registry",
        default=os.environ.get(
            "CAPTURE_SERVICE_REGISTRY",
            "/tmp/capture_service_streams.json",
        ),
        help="capture_service_cpp registry path",
    )
    parser.add_argument(
        "--stream",
        default=os.environ.get("CAPTURE_IMU_STREAM", "imu0"),
        help="normalized IMU_F32_LE stream ID",
    )
    parser.add_argument(
        "--output",
        default="",
        help="JSON report path; defaults to /tmp/capture_imu_mount_calibration.json",
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
        help="countdown before recording",
    )
    parser.add_argument(
        "--axes-max-error-deg",
        type=float,
        default=8.0,
        help=(
            "maximum angular difference from a signed 90-degree mapping before "
            "quaternion_xyzw is recommended"
        ),
    )
    parser.add_argument(
        "--self-test",
        action="store_true",
        help="run deterministic math/parser-independent tests and exit",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if args.self_test:
        return run_self_test()
    if args.poll_ms <= 0.0:
        raise RuntimeError("--poll-ms must be greater than zero")
    if args.countdown < 0:
        raise RuntimeError("--countdown cannot be negative")
    if not (0.1 <= args.axes_max_error_deg <= 30.0):
        raise RuntimeError("--axes-max-error-deg must be between 0.1 and 30")

    registry_path = Path(args.registry).expanduser()
    info = read_registry(registry_path, args.stream)
    phases = make_phases()
    duration_sec = phases[-1].end_sec

    print("=" * 80)
    print("capture_service_cpp IMU mount calibration")
    print("Reference: verified XREAL Ultra IMU axis layout")
    print(f"Registry:  {info.registry_path}")
    print(f"Stream:    {info.stream_id} ({info.format_name})")
    print(f"SHM:       {shm_path_for_name(info.shm_name)}")
    print(f"Namespace: {info.namespace or '<unset>'}")
    print(f"Profile:   {info.profile or '<unset>'}")
    print(f"Config:    {info.config_path or '<unknown>'}")
    print(f"Duration:  {duration_sec:.0f} seconds")
    print()
    print("REFERENCE MOTIONS:")
    print("  pitch up   -> +X")
    print("  yaw right  -> -Y")
    print("  roll right -> -Z")
    print()
    print("IMPORTANT:")
    print("  1. Remove/comment imu.transform in the tested capture config.")
    print("  2. Restart capture_service_cpp before starting this calibration.")
    print("  3. During each motion phase rotate once in the requested direction and HOLD.")
    print("  4. Do not rotate back until the following neutral phase begins.")

    transform_warning = possible_transform_warning(info.config_path)
    if transform_warning:
        print("[WARNING] " + transform_warning)
    print("=" * 80)

    if args.countdown:
        print(f"Recording starts in {args.countdown} seconds...")
        for remaining in range(args.countdown, 0, -1):
            print(f"  {remaining}", flush=True)
            time.sleep(1.0)

    samples: List[ImuSample] = []
    dropped_sequences = 0
    invalid_slots = 0
    last_phase_name = ""
    last_status_print = 0.0

    with CaptureImuShmReader(info) as reader:
        # Ignore historical ring entries. Calibration starts with the next sample.
        last_sequence = reader.latest_sequence()
        start = time.monotonic()

        while True:
            now = time.monotonic()
            elapsed = now - start
            if elapsed >= duration_sec:
                break

            phase = phase_at(phases, elapsed)
            if phase.name != last_phase_name:
                print()
                print("\a" + "#" * 80)
                print(
                    f"[{phase.start_sec:02.0f}-{phase.end_sec:02.0f} s] "
                    f"{phase.instruction}"
                )
                print("#" * 80, flush=True)
                last_phase_name = phase.name

            latest = reader.latest_sequence()
            if latest < last_sequence:
                raise RuntimeError(
                    "SHM sequence moved backwards; capture_service_cpp was probably restarted"
                )
            if latest > last_sequence:
                first_available = max(1, latest - info.slot_count + 1)
                next_sequence = last_sequence + 1
                if next_sequence < first_available:
                    dropped_sequences += first_available - next_sequence
                    next_sequence = first_available

                for sequence in range(next_sequence, latest + 1):
                    sample = reader.read_sequence(sequence, elapsed)
                    if sample is None:
                        invalid_slots += 1
                        continue
                    samples.append(sample)
                last_sequence = latest

            if now - last_status_print >= 0.25:
                remaining = max(0.0, phase.end_sec - elapsed)
                if samples:
                    gyro = samples[-1].gyro
                    print(
                        f"\r  {remaining:4.1f} s remaining | "
                        f"samples={len(samples):5d} | "
                        f"gyro X={gyro[0]:+6.2f} "
                        f"Y={gyro[1]:+6.2f} "
                        f"Z={gyro[2]:+6.2f} rad/s",
                        end="",
                        flush=True,
                    )
                else:
                    print(
                        f"\r  {remaining:4.1f} s remaining | waiting for IMU samples",
                        end="",
                        flush=True,
                    )
                last_status_print = now

            time.sleep(max(0.0005, args.poll_ms / 1000.0))

    print("\n\a")

    notes: List[str] = []
    if len(samples) < 100:
        notes.append(f"too few samples: {len(samples)}")

    sample_rate_hz = len(samples) / duration_sec if duration_sec > 0.0 else 0.0
    timestamp_dt = median_positive_timestamp_dt(samples)
    fallback_dt = timestamp_dt if timestamp_dt is not None else (
        1.0 / sample_rate_hz if sample_rate_hz > 1.0 else 0.005
    )

    still_names = {"still_start", "still_end"}
    still_gyro: List[Vector3] = []
    for phase in phases:
        if phase.name in still_names:
            still_gyro.extend(sample.gyro for sample in phase_samples(samples, phase))
    bias = median_vector(still_gyro)
    noise_sigma = robust_noise_sigma(still_gyro, bias)
    noise_vector_sigma = v_norm(noise_sigma)
    active_threshold = clamp(max(0.035, noise_vector_sigma * 6.0), 0.035, 0.20)

    if len(still_gyro) < 20:
        notes.append("too few still samples for gyro bias/noise estimation")
    if v_norm(bias) > 0.25:
        notes.append(
            f"unusually large gyro bias ({v_norm(bias):.3f} rad/s); keep the device still at start/end"
        )
    if noise_vector_sigma > 0.04:
        notes.append(
            f"high still gyro noise ({noise_vector_sigma:.3f} rad/s); calibration may be unreliable"
        )

    phase_by_name = {phase.name: phase for phase in phases}
    roll = analyze_motion(
        "roll_right",
        phase_samples(samples, phase_by_name["roll_right"]),
        bias,
        active_threshold,
        fallback_dt,
    )
    pitch = analyze_motion(
        "pitch_up",
        phase_samples(samples, phase_by_name["pitch_up"]),
        bias,
        active_threshold,
        fallback_dt,
    )
    yaw = analyze_motion(
        "yaw_right",
        phase_samples(samples, phase_by_name["yaw_right"]),
        bias,
        active_threshold,
        fallback_dt,
    )

    recommendation = build_recommendation(
        roll,
        pitch,
        yaw,
        args.axes_max_error_deg,
    )
    all_notes = notes + list(recommendation.notes)
    success = recommendation.success and not notes

    # Preserve a mathematically valid recommendation in the report even if
    # environmental warnings force a non-zero exit status.
    if recommendation.success and notes:
        success = False

    output_path = (
        Path(args.output).expanduser()
        if args.output
        else Path("/tmp/capture_imu_mount_calibration.json")
    )
    output_path.parent.mkdir(parents=True, exist_ok=True)

    yaml_block = yaml_for_recommendation(recommendation)
    report: Dict[str, object] = {
        "format": REPORT_FORMAT,
        "success": success,
        "source": "capture_service_cpp_shm",
        "reference": {
            "name": "xreal_ultra_verified_layout",
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
        "registry": str(info.registry_path),
        "stream": info.stream_id,
        "shm": str(shm_path_for_name(info.shm_name)),
        "namespace": info.namespace,
        "profile": info.profile,
        "config_path": info.config_path,
        "sample_count": len(samples),
        "estimated_sample_rate_hz": sample_rate_hz,
        "median_timestamp_dt_sec": timestamp_dt,
        "dropped_sequences": dropped_sequences,
        "invalid_slots": invalid_slots,
        "gyro_bias_rad_s": list(bias),
        "gyro_noise_sigma_rad_s": list(noise_sigma),
        "active_threshold_rad_s": active_threshold,
        "motions": {},
        "recommendation": {
            "mode": recommendation.mode,
            "rotation_matrix_output_from_source": (
                compact_matrix(recommendation.rotation_matrix)
                if recommendation.rotation_matrix is not None
                else None
            ),
            "axes": list(recommendation.axes) if recommendation.axes else None,
            "quaternion_xyzw": (
                [round(value, 9) for value in recommendation.quaternion_xyzw]
                if recommendation.quaternion_xyzw is not None
                else None
            ),
            "axes_residual_deg": recommendation.axes_residual_deg,
            "identity_residual_deg": recommendation.identity_residual_deg,
            "yaml": yaml_block,
        },
        "notes": all_notes,
    }

    for motion in (roll, pitch, yaw):
        report["motions"][motion.name] = {
            "sample_count": motion.sample_count,
            "active_sample_count": motion.active_sample_count,
            "integrated_duration_sec": motion.integrated_duration_sec,
            "signed_integral_rad": motion.signed_integral_rad,
            "absolute_angle_rad": motion.absolute_angle_rad,
            "absolute_angle_deg": math.degrees(motion.absolute_angle_rad),
            "peak_rad_s": motion.peak_rad_s,
            "coherence": motion.coherence,
            "direction_raw": list(motion.direction_raw) if motion.direction_raw else None,
        }

    with output_path.open("w", encoding="utf-8") as file:
        json.dump(report, file, ensure_ascii=False, indent=2)
        file.write("\n")

    print("=" * 80)
    print("RESULT")
    print("=" * 80)
    print(f"samples={len(samples)}")
    print(f"estimated_sample_rate_hz={sample_rate_hz:.1f}")
    print(f"dropped_sequences={dropped_sequences}")
    print(f"invalid_slots={invalid_slots}")
    print(f"gyro_bias_rad_s={compact_vector(bias)}")
    print(f"gyro_noise_sigma_rad_s={compact_vector(noise_sigma)}")
    print(f"active_threshold_rad_s={active_threshold:.4f}")

    for motion in (roll, pitch, yaw):
        direction = (
            compact_vector(motion.direction_raw)
            if motion.direction_raw is not None
            else "<none>"
        )
        print(
            f"{motion.name}: active={motion.active_sample_count}/{motion.sample_count} "
            f"angle={math.degrees(motion.absolute_angle_rad):.1f} deg "
            f"coherence={motion.coherence * 100.0:.1f}% "
            f"raw_direction={direction}"
        )

    if recommendation.rotation_matrix is not None:
        print("rotation_matrix_output_from_source=")
        for row in recommendation.rotation_matrix:
            print("  " + compact_vector(row, digits=6))
    if recommendation.axes_residual_deg is not None:
        print(f"nearest_axes_residual_deg={recommendation.axes_residual_deg:.2f}")
    if recommendation.identity_residual_deg is not None:
        print(f"identity_residual_deg={recommendation.identity_residual_deg:.2f}")

    if success:
        print()
        print("CALIBRATION SUCCESSFUL")
        print("Copy this into the capture_service_cpp profile:")
        print("-" * 80)
        print(yaml_block)
        print("-" * 80)
        print("Restart capture_service_cpp after changing the config.")
    else:
        print()
        print("CALIBRATION NOT RELIABLE - do not change imu.transform yet.")

    if all_notes:
        print("notes=")
        for note in all_notes:
            print(f"  - {note}")
    else:
        print("notes=[]")
    print(f"json_report={output_path}")
    print("=" * 80)

    return 0 if success else 2


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except KeyboardInterrupt:
        print("\nCalibration interrupted.", file=sys.stderr)
        raise SystemExit(130)
    except Exception as exc:
        print(f"[ERROR] {exc}", file=sys.stderr)
        raise SystemExit(1)
