#!/usr/bin/env python3
"""Calibrate and verify the neutral HMD orientation offset.

IMU axis assignment, axis signs, handedness, and sensor mounting rotation must
already be normalized by calibrate_imu_axes.py and capture_service_cpp. This
tool therefore does not recover or remap IMU axes. It only records a still HMD
pose and computes the world-space quaternion needed to make that pose level.

Two modes are available:

* ``level``: preserve the current horizontal heading and remove tilt/roll;
* ``full-neutral``: make the current pose the complete identity orientation.

The generated runtime block always uses ``multiply_order: pre``. This corrects
the neutral world orientation without rotating the HMD's local pitch/yaw/roll
axes. When --write is used, only streams.hmd.orientation_offset is updated.

python3 calibrate_hmd_orientation_offset.py \
  --registry /tmp/tracking_streams.json \
  --stream hmd_pose \
  --mode level

"""

from __future__ import annotations

import argparse
import json
import math
import mmap
import os
import shutil
import statistics
import struct
import sys
import tempfile
import time
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path
from typing import Iterable, List, Optional, Sequence, Tuple


# Packed xr_runtime::RingSlotHeaderV1 leading fields.
SLOT_HEADER_PREFIX = struct.Struct("<QQQQII")
# Packed xr_runtime::HmdPoseF64V1, sizeof == 160 bytes.
HMD_POSE_STRUCT = struct.Struct("<IIQQQQ13dIIfI")
HMD_POSE_SIZE = 160
HMD_FLAG_POSE_VALID = 1 << 0
HMD_FLAG_ANGULAR_VELOCITY_VALID = 1 << 2
EXPECTED_FORMAT = "HMD_POSE_F64_LE"
REPORT_FORMAT = "XR_HMD_NEUTRAL_ORIENTATION_CALIBRATION_V3"

Vector3 = Tuple[float, float, float]
Quaternion = Tuple[float, float, float, float]  # xyzw
Matrix3 = Tuple[Tuple[float, float, float], ...]


@dataclass(frozen=True)
class RegistryInfo:
    registry_path: Path
    stream_id: str
    shm_name: str
    format_name: str
    payload_size: int
    slot_count: int
    header_size: int
    slot_header_size: int
    slot_stride: int
    frame_id: str


@dataclass(frozen=True)
class HmdSample:
    sequence: int
    timestamp_ns: int
    source_timestamp_ns: int
    quaternion_xyzw: Quaternion
    angular_velocity: Vector3
    tracking_status: int
    flags: int
    confidence: float


@dataclass(frozen=True)
class OffsetConfig:
    enabled: bool
    multiply_order: str
    quaternion_xyzw: Quaternion


@dataclass(frozen=True)
class CalibrationResult:
    mean_source_xyzw: Quaternion
    mean_basis_xyzw: Quaternion
    target_xyzw: Quaternion
    orientation_offset_xyzw: Quaternion
    predicted_output_xyzw: Quaternion
    max_sample_deviation_deg: float
    rms_sample_deviation_deg: float
    tilt_residual_deg: float
    full_residual_deg: float
    sample_count: int
    angular_speed_p95_rad_s: Optional[float]


@dataclass(frozen=True)
class TimedHmdSample:
    elapsed_sec: float
    sample: HmdSample


@dataclass(frozen=True)
class GuidedPhase:
    name: str
    instruction: str
    start_sec: float
    end_sec: float
    kind: str


@dataclass(frozen=True)
class GuidedMotionResult:
    name: str
    desired_axis: Vector3
    measured_axis: Vector3
    corrected_axis: Vector3
    motion_angle_deg: float
    axis_error_deg: float
    candidate_fit_residual_deg: float
    neutral_sample_count: int
    endpoint_sample_count: int
    neutral_max_deviation_deg: float
    endpoint_max_deviation_deg: float


@dataclass(frozen=True)
class GuidedCalibrationResult:
    calibration: CalibrationResult
    recommended_offset_xyzw: Quaternion
    applied_offset_xyzw: Quaternion
    offset_difference_deg: float
    motions: Tuple[GuidedMotionResult, ...]
    raw_basis_determinant: float
    measured_pair_angles_deg: Tuple[float, float, float]
    candidate_fit_residual_max_deg: float


def clamp(value: float, low: float, high: float) -> float:
    return max(low, min(high, value))


def finite(values: Iterable[float]) -> bool:
    return all(math.isfinite(value) for value in values)


def v_dot(a: Vector3, b: Vector3) -> float:
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]


def v_cross(a: Vector3, b: Vector3) -> Vector3:
    return (
        a[1] * b[2] - a[2] * b[1],
        a[2] * b[0] - a[0] * b[2],
        a[0] * b[1] - a[1] * b[0],
    )


def v_norm(v: Vector3) -> float:
    return math.sqrt(v_dot(v, v))


def v_scale(v: Vector3, scale: float) -> Vector3:
    return (v[0] * scale, v[1] * scale, v[2] * scale)


def v_neg(v: Vector3) -> Vector3:
    return (-v[0], -v[1], -v[2])


def v_angle_deg(a: Vector3, b: Vector3) -> float:
    denominator = v_norm(a) * v_norm(b)
    if denominator <= 1e-12:
        return 180.0
    cosine = clamp(v_dot(a, b) / denominator, -1.0, 1.0)
    return math.degrees(math.acos(cosine))


def v_normalize(v: Vector3) -> Vector3:
    norm = v_norm(v)
    if not math.isfinite(norm) or norm <= 1e-12:
        raise ValueError("cannot normalize a zero/non-finite vector")
    return v_scale(v, 1.0 / norm)


def q_normalize(q: Quaternion, canonical: bool = False) -> Quaternion:
    norm = math.sqrt(sum(value * value for value in q))
    if not math.isfinite(norm) or norm <= 1e-12:
        raise ValueError("cannot normalize a zero/non-finite quaternion")
    out = tuple(value / norm for value in q)
    if canonical and out[3] < 0.0:
        out = tuple(-value for value in out)
    return out  # type: ignore[return-value]


def q_conj(q: Quaternion) -> Quaternion:
    return (-q[0], -q[1], -q[2], q[3])


def q_mul_raw(a: Quaternion, b: Quaternion) -> Quaternion:
    ax, ay, az, aw = a
    bx, by, bz, bw = b
    return (
        aw * bx + ax * bw + ay * bz - az * by,
        aw * by - ax * bz + ay * bw + az * bx,
        aw * bz + ax * by - ay * bx + az * bw,
        aw * bw - ax * bx - ay * by - az * bz,
    )


def q_mul(a: Quaternion, b: Quaternion) -> Quaternion:
    return q_normalize(q_mul_raw(a, b))


def q_dot(a: Quaternion, b: Quaternion) -> float:
    return sum(left * right for left, right in zip(a, b))


def q_angular_distance_deg(a: Quaternion, b: Quaternion) -> float:
    dot = clamp(abs(q_dot(q_normalize(a), q_normalize(b))), 0.0, 1.0)
    return math.degrees(2.0 * math.acos(dot))


def q_from_axis_angle(axis: Vector3, radians: float) -> Quaternion:
    axis_n = v_normalize(axis)
    half = radians * 0.5
    scale = math.sin(half)
    return q_normalize(
        (axis_n[0] * scale, axis_n[1] * scale, axis_n[2] * scale, math.cos(half))
    )


def q_from_euler_basis_xyz(rx: float, ry: float, rz: float) -> Quaternion:
    qx = q_from_axis_angle((1.0, 0.0, 0.0), rx)
    qy = q_from_axis_angle((0.0, 1.0, 0.0), ry)
    qz = q_from_axis_angle((0.0, 0.0, 1.0), rz)
    # Match coordinate_util.cpp: qz * qy * qx.
    return q_mul(qz, q_mul(qy, qx))


def q_rotate(q_raw: Quaternion, v: Vector3) -> Vector3:
    q = q_normalize(q_raw)
    p = (v[0], v[1], v[2], 0.0)
    rotated = q_mul_raw(q_mul_raw(q, p), q_conj(q))
    return (rotated[0], rotated[1], rotated[2])


def q_apply_basis(basis: Quaternion, source: Quaternion) -> Quaternion:
    return q_mul(q_mul(basis, source), q_conj(basis))


def q_apply_offset(base: Quaternion, offset: Quaternion, multiply_order: str) -> Quaternion:
    if multiply_order in ("post", "local"):
        return q_mul(base, offset)
    if multiply_order in ("pre", "world"):
        return q_mul(offset, base)
    raise ValueError(f"unsupported multiply order: {multiply_order}")


def q_average(values: Sequence[Quaternion]) -> Quaternion:
    if not values:
        raise ValueError("cannot average an empty quaternion list")
    reference = q_normalize(values[0])
    accum = [0.0, 0.0, 0.0, 0.0]
    for value in values:
        q = q_normalize(value)
        if q_dot(q, reference) < 0.0:
            q = tuple(-component for component in q)  # type: ignore[assignment]
        for index in range(4):
            accum[index] += q[index]
    return q_normalize(tuple(accum), canonical=True)  # type: ignore[arg-type]


def q_to_axis_angle(q_raw: Quaternion) -> Tuple[Vector3, float]:
    q = q_normalize(q_raw, canonical=True)
    angle = 2.0 * math.acos(clamp(q[3], -1.0, 1.0))
    sin_half = math.sqrt(max(0.0, 1.0 - q[3] * q[3]))
    if sin_half <= 1e-8 or angle <= 1e-8:
        raise ValueError("relative orientation contains no usable rotation")
    axis = (q[0] / sin_half, q[1] / sin_half, q[2] / sin_half)
    return v_normalize(axis), angle


def matrix_determinant(matrix: Matrix3) -> float:
    a, b, c = matrix[0]
    d, e, f = matrix[1]
    g, h, i = matrix[2]
    return (
        a * (e * i - f * h)
        - b * (d * i - f * g)
        + c * (d * h - e * g)
    )


def matrix_transpose(matrix: Matrix3) -> Matrix3:
    return tuple(
        tuple(float(matrix[column][row]) for column in range(3))
        for row in range(3)
    )


def matrix_inverse(matrix: Matrix3) -> Matrix3:
    a, b, c = matrix[0]
    d, e, f = matrix[1]
    g, h, i = matrix[2]
    determinant = matrix_determinant(matrix)
    if not math.isfinite(determinant) or abs(determinant) <= 1e-10:
        raise ValueError("measured motion basis is singular")
    scale = 1.0 / determinant
    return (
        (
            (e * i - f * h) * scale,
            (c * h - b * i) * scale,
            (b * f - c * e) * scale,
        ),
        (
            (f * g - d * i) * scale,
            (a * i - c * g) * scale,
            (c * d - a * f) * scale,
        ),
        (
            (d * h - e * g) * scale,
            (b * g - a * h) * scale,
            (a * e - b * d) * scale,
        ),
    )


def matrix_max_error(a: Matrix3, b: Matrix3) -> float:
    return max(
        abs(a[row][column] - b[row][column])
        for row in range(3)
        for column in range(3)
    )


def matrix_average(a: Matrix3, b: Matrix3) -> Matrix3:
    return tuple(
        tuple((a[row][column] + b[row][column]) * 0.5 for column in range(3))
        for row in range(3)
    )


def matrix_from_columns(x_axis: Vector3, y_axis: Vector3, z_axis: Vector3) -> Matrix3:
    return (
        (x_axis[0], y_axis[0], z_axis[0]),
        (x_axis[1], y_axis[1], z_axis[1]),
        (x_axis[2], y_axis[2], z_axis[2]),
    )


def matrix_column(matrix: Matrix3, column: int) -> Vector3:
    return (matrix[0][column], matrix[1][column], matrix[2][column])


def closest_rotation(matrix: Matrix3) -> Matrix3:
    """Return the closest proper rotation using Newton polar iteration."""

    if matrix_determinant(matrix) <= 0.0:
        raise ValueError(
            "guided motions form a reflected basis; check motion directions"
        )
    result = matrix
    for _ in range(24):
        inverse_transpose = matrix_transpose(matrix_inverse(result))
        updated = matrix_average(result, inverse_transpose)
        if matrix_max_error(updated, result) < 1e-12:
            result = updated
            break
        result = updated
    determinant = matrix_determinant(result)
    if not 0.999 <= determinant <= 1.001:
        raise ValueError(
            f"failed to fit a proper mount rotation; determinant={determinant:.6f}"
        )
    return result


def q_to_matrix(q_raw: Quaternion) -> Matrix3:
    x, y, z, w = q_normalize(q_raw)
    return (
        (
            1.0 - 2.0 * (y * y + z * z),
            2.0 * (x * y - z * w),
            2.0 * (x * z + y * w),
        ),
        (
            2.0 * (x * y + z * w),
            1.0 - 2.0 * (x * x + z * z),
            2.0 * (y * z - x * w),
        ),
        (
            2.0 * (x * z - y * w),
            2.0 * (y * z + x * w),
            1.0 - 2.0 * (x * x + y * y),
        ),
    )


def matrix_to_q(matrix: Matrix3) -> Quaternion:
    m00, m01, m02 = matrix[0]
    m10, m11, m12 = matrix[1]
    m20, m21, m22 = matrix[2]
    trace = m00 + m11 + m22

    if trace > 0.0:
        scale = math.sqrt(trace + 1.0) * 2.0
        w = 0.25 * scale
        x = (m21 - m12) / scale
        y = (m02 - m20) / scale
        z = (m10 - m01) / scale
    elif m00 > m11 and m00 > m22:
        scale = math.sqrt(max(0.0, 1.0 + m00 - m11 - m22)) * 2.0
        w = (m21 - m12) / scale
        x = 0.25 * scale
        y = (m01 + m10) / scale
        z = (m02 + m20) / scale
    elif m11 > m22:
        scale = math.sqrt(max(0.0, 1.0 + m11 - m00 - m22)) * 2.0
        w = (m02 - m20) / scale
        x = (m01 + m10) / scale
        y = 0.25 * scale
        z = (m12 + m21) / scale
    else:
        scale = math.sqrt(max(0.0, 1.0 + m22 - m00 - m11)) * 2.0
        w = (m10 - m01) / scale
        x = (m02 + m20) / scale
        y = (m12 + m21) / scale
        z = 0.25 * scale
    return q_normalize((x, y, z, w), canonical=True)


def q_to_euler_xyz_deg(q: Quaternion) -> Vector3:
    matrix = q_to_matrix(q)
    ry = math.asin(clamp(-matrix[2][0], -1.0, 1.0))
    cos_ry = math.cos(ry)
    if abs(cos_ry) > 1e-8:
        rx = math.atan2(matrix[2][1], matrix[2][2])
        rz = math.atan2(matrix[1][0], matrix[0][0])
    else:
        rx = math.atan2(-matrix[1][2], matrix[1][1])
        rz = 0.0
    return (math.degrees(rx), math.degrees(ry), math.degrees(rz))


def level_target_from_orientation(q_mean: Quaternion) -> Quaternion:
    """Return a Y-up yaw-only target preserving the current horizontal heading.

    The forward vector can be nearly vertical when the source frame has a 90°
    mount error, so this function falls back to the projected right vector.
    """

    world_up: Vector3 = (0.0, 1.0, 0.0)
    forward = q_rotate(q_mean, (0.0, 0.0, -1.0))
    right = q_rotate(q_mean, (1.0, 0.0, 0.0))

    forward_horizontal = (forward[0], 0.0, forward[2])
    right_horizontal = (right[0], 0.0, right[2])
    forward_norm = v_norm(forward_horizontal)
    right_norm = v_norm(right_horizontal)

    if forward_norm >= right_norm and forward_norm > 1e-6:
        horizontal_forward = v_scale(forward_horizontal, 1.0 / forward_norm)
        back = v_scale(horizontal_forward, -1.0)
        horizontal_right = v_normalize(v_cross(world_up, back))
    elif right_norm > 1e-6:
        horizontal_right = v_scale(right_horizontal, 1.0 / right_norm)
        back = v_normalize(v_cross(horizontal_right, world_up))
    else:
        raise ValueError("cannot derive horizontal heading from neutral orientation")

    # Rotation matrix columns are the world-space local X/right, Y/up, Z/back axes.
    matrix: Matrix3 = (
        (horizontal_right[0], world_up[0], back[0]),
        (horizontal_right[1], world_up[1], back[1]),
        (horizontal_right[2], world_up[2], back[2]),
    )
    return matrix_to_q(matrix)


def tilt_from_level_deg(q: Quaternion) -> float:
    up = q_rotate(q, (0.0, 1.0, 0.0))
    cosine = clamp(v_dot(v_normalize(up), (0.0, 1.0, 0.0)), -1.0, 1.0)
    return math.degrees(math.acos(cosine))


def percentile(values: Sequence[float], fraction: float) -> Optional[float]:
    if not values:
        return None
    ordered = sorted(values)
    index = clamp(fraction, 0.0, 1.0) * (len(ordered) - 1)
    low = int(math.floor(index))
    high = int(math.ceil(index))
    if low == high:
        return ordered[low]
    weight = index - low
    return ordered[low] * (1.0 - weight) + ordered[high] * weight


def make_guided_phases(scale: float = 1.0) -> Tuple[GuidedPhase, ...]:
    if scale <= 0.0:
        raise ValueError("guided phase scale must be greater than zero")

    def t(value: float) -> float:
        return value * scale

    return (
        GuidedPhase(
            "neutral_start",
            "LOOK STRAIGHT: hold the intended neutral head pose completely still.",
            t(0.0),
            t(4.0),
            "neutral",
        ),
        GuidedPhase(
            "pitch_up",
            "PITCH UP: raise your gaze smoothly, then HOLD the raised pose.",
            t(4.0),
            t(10.0),
            "motion",
        ),
        GuidedPhase(
            "neutral_pitch",
            "Return to the same straight neutral pose and hold still.",
            t(10.0),
            t(13.0),
            "neutral",
        ),
        GuidedPhase(
            "yaw_right",
            "YAW RIGHT: turn your head smoothly to the right, then HOLD.",
            t(13.0),
            t(19.0),
            "motion",
        ),
        GuidedPhase(
            "neutral_yaw",
            "Return to the same straight neutral pose and hold still.",
            t(19.0),
            t(22.0),
            "neutral",
        ),
        GuidedPhase(
            "roll_right",
            "ROLL RIGHT: tilt your head toward the right shoulder, then HOLD.",
            t(22.0),
            t(28.0),
            "motion",
        ),
        GuidedPhase(
            "neutral_end",
            "Return to the straight neutral pose and hold still until completion.",
            t(28.0),
            t(32.0),
            "neutral",
        ),
    )


def guided_phase_at(phases: Sequence[GuidedPhase], elapsed: float) -> GuidedPhase:
    for phase in phases:
        if phase.start_sec <= elapsed < phase.end_sec:
            return phase
    return phases[-1]


def read_registry(path: Path, stream_id: str) -> RegistryInfo:
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except FileNotFoundError as exc:
        raise RuntimeError(
            f"tracking registry not found: {path}; start the HMD backend first"
        ) from exc
    except json.JSONDecodeError as exc:
        raise RuntimeError(f"invalid tracking registry JSON {path}: {exc}") from exc

    streams = data.get("streams")
    if not isinstance(streams, dict) or stream_id not in streams:
        available = ", ".join(sorted(streams)) if isinstance(streams, dict) else "<none>"
        raise RuntimeError(
            f"stream {stream_id!r} not found in {path}; available: {available}"
        )
    entry = streams[stream_id]
    if not isinstance(entry, dict):
        raise RuntimeError(f"registry entry {stream_id!r} is not an object")

    payload_size = int(entry.get("payload_size", 0))
    slot_header_size = int(entry.get("slot_header_size", 128))
    slot_stride = int(entry.get("slot_stride", 0))
    if slot_stride <= 0:
        slot_stride = slot_header_size + payload_size
    info = RegistryInfo(
        registry_path=path,
        stream_id=stream_id,
        shm_name=str(entry.get("shm_name", "")),
        format_name=str(entry.get("format_name", entry.get("format", ""))),
        payload_size=payload_size,
        slot_count=int(entry.get("slot_count", 0)),
        header_size=int(entry.get("header_size", 4096)),
        slot_header_size=slot_header_size,
        slot_stride=slot_stride,
        frame_id=str(entry.get("frame_id", "tracking_world")),
    )
    if not info.shm_name:
        raise RuntimeError(f"stream {stream_id!r} has no shm_name")
    if info.format_name != EXPECTED_FORMAT:
        raise RuntimeError(
            f"stream {stream_id!r} has format {info.format_name!r}; expected {EXPECTED_FORMAT!r}"
        )
    if info.payload_size < HMD_POSE_SIZE:
        raise RuntimeError(
            f"stream payload_size={info.payload_size}; expected at least {HMD_POSE_SIZE}"
        )
    if info.slot_count <= 0 or info.header_size <= 0 or info.slot_header_size < 40:
        raise RuntimeError("invalid SHM ring dimensions in registry")
    if info.slot_stride < info.slot_header_size + info.payload_size:
        raise RuntimeError("invalid slot_stride in registry")
    return info


def shm_path(name: str) -> Path:
    return Path("/dev/shm") / name.lstrip("/")


class HmdPoseShmReader:
    def __init__(self, info: RegistryInfo):
        self.info = info
        self.path = shm_path(info.shm_name)
        if not self.path.exists():
            raise RuntimeError(f"SHM object not found: {self.path}")
        expected_size = info.header_size + info.slot_count * info.slot_stride
        if self.path.stat().st_size < expected_size:
            raise RuntimeError(
                f"SHM object is too small: {self.path.stat().st_size} < {expected_size}"
            )
        self._file = self.path.open("rb")
        self._mm = mmap.mmap(self._file.fileno(), 0, access=mmap.ACCESS_READ)
        self._latest_sequence_offset = self._detect_latest_sequence_offset()

    def _detect_latest_sequence_offset(self) -> int:
        if len(self._mm) < 48:
            raise RuntimeError("SHM header is smaller than 48 bytes")
        magic = bytes(self._mm[:8])
        if magic == b"CAPSHM1\0":
            return 40
        if magic in (b"HTRKRG1\0", b"RTPOSE1\0"):
            return 36
        raise RuntimeError(f"unsupported HMD SHM magic: {magic!r}")

    def close(self) -> None:
        self._mm.close()
        self._file.close()

    def __enter__(self) -> "HmdPoseShmReader":
        return self

    def __exit__(self, exc_type, exc, traceback) -> None:
        self.close()

    def latest_sequence(self) -> int:
        return struct.unpack_from("<Q", self._mm, self._latest_sequence_offset)[0]

    def read_sequence(self, sequence: int) -> Optional[HmdSample]:
        if sequence <= 0:
            return None
        latest = self.latest_sequence()
        if latest == 0 or sequence > latest or latest - sequence >= self.info.slot_count:
            return None

        slot_index = (sequence - 1) % self.info.slot_count
        slot_offset = self.info.header_size + slot_index * self.info.slot_stride
        payload_offset = slot_offset + self.info.slot_header_size
        if payload_offset + HMD_POSE_SIZE > len(self._mm):
            return None

        header1 = SLOT_HEADER_PREFIX.unpack_from(self._mm, slot_offset)
        seq_begin, seq_end, timestamp_ns, source_timestamp_ns, payload_size, _flags = header1
        if seq_begin != seq_end or seq_begin == 0 or (seq_begin & 1):
            return None
        actual_sequence = seq_end // 2
        if actual_sequence != sequence or payload_size < HMD_POSE_SIZE:
            return None

        payload = bytes(self._mm[payload_offset : payload_offset + HMD_POSE_SIZE])
        header2 = SLOT_HEADER_PREFIX.unpack_from(self._mm, slot_offset)
        if header1[:2] != header2[:2]:
            return None

        values = HMD_POSE_STRUCT.unpack(payload)
        version, size_bytes = values[0], values[1]
        if version != 1 or size_bytes < HMD_POSE_SIZE:
            return None
        quaternion = (values[10], values[11], values[12], values[9])
        angular_velocity = (values[16], values[17], values[18])
        tracking_status = int(values[19])
        flags = int(values[20])
        confidence = float(values[21])
        if not finite((*quaternion, *angular_velocity, confidence)):
            return None
        try:
            quaternion = q_normalize(quaternion)
        except ValueError:
            return None
        return HmdSample(
            sequence=actual_sequence,
            timestamp_ns=int(values[3] or timestamp_ns),
            source_timestamp_ns=int(values[4] or source_timestamp_ns),
            quaternion_xyzw=quaternion,
            angular_velocity=angular_velocity,
            tracking_status=tracking_status,
            flags=flags,
            confidence=confidence,
        )


def parse_rotation_object(value: object) -> Vector3:
    if isinstance(value, list):
        if len(value) != 3:
            raise RuntimeError("rotation array must contain three values")
        result = tuple(float(item) for item in value)
    elif isinstance(value, dict):
        result = (
            float(value.get("rx_deg", value.get("x", 0.0))),
            float(value.get("ry_deg", value.get("y", 0.0))),
            float(value.get("rz_deg", value.get("z", 0.0))),
        )
    else:
        raise RuntimeError("rotation must be an object or array")
    if not finite(result):
        raise RuntimeError("rotation values must be finite")
    return result  # type: ignore[return-value]


def load_basis(config: dict, stream_name: str) -> Quaternion:
    try:
        stream = config["streams"][stream_name]
    except (KeyError, TypeError) as exc:
        raise RuntimeError(f"transform config has no streams.{stream_name}") from exc
    block = stream.get("orientation_transform", {})
    if not isinstance(block, dict) or not block.get("enabled", False):
        return (0.0, 0.0, 0.0, 1.0)
    rotation = block.get("basis_rotation", block.get("basis_rotation_deg", {}))
    rx, ry, rz = parse_rotation_object(rotation)
    return q_from_euler_basis_xyz(
        math.radians(rx), math.radians(ry), math.radians(rz)
    )


def load_offset(config: dict, stream_name: str) -> OffsetConfig:
    try:
        stream = config["streams"][stream_name]
    except (KeyError, TypeError) as exc:
        raise RuntimeError(f"transform config has no streams.{stream_name}") from exc
    block = stream.get("orientation_offset", {})
    if not isinstance(block, dict):
        raise RuntimeError(f"streams.{stream_name}.orientation_offset must be an object")
    enabled = bool(block.get("enabled", False))
    order = str(block.get("multiply_order", "post"))
    if order not in ("post", "local", "pre", "world"):
        raise RuntimeError(
            f"streams.{stream_name}.orientation_offset.multiply_order is invalid: {order}"
        )
    has_q = "quaternion_xyzw" in block
    has_rotation = "rotation_deg" in block or "rotation" in block
    if has_q and has_rotation:
        raise RuntimeError(
            f"streams.{stream_name}.orientation_offset contains both quaternion and rotation"
        )
    if has_q:
        value = block["quaternion_xyzw"]
        if not isinstance(value, list) or len(value) != 4:
            raise RuntimeError("orientation_offset.quaternion_xyzw must contain four values")
        q = q_normalize(tuple(float(item) for item in value))  # type: ignore[arg-type]
    elif has_rotation:
        rotation = block.get("rotation_deg", block.get("rotation"))
        rx, ry, rz = parse_rotation_object(rotation)
        q = q_from_euler_basis_xyz(
            math.radians(rx), math.radians(ry), math.radians(rz)
        )
    else:
        q = (0.0, 0.0, 0.0, 1.0)
    return OffsetConfig(enabled=enabled, multiply_order=order, quaternion_xyzw=q)


def collect_samples(
    info: RegistryInfo,
    duration_sec: float,
    poll_ms: float,
    countdown: int,
) -> Tuple[List[HmdSample], int, int]:
    if countdown:
        print(f"Recording starts in {countdown} seconds. Look straight ahead and hold still.")
        for remaining in range(countdown, 0, -1):
            print(f"  {remaining}", flush=True)
            time.sleep(1.0)

    samples: List[HmdSample] = []
    dropped = 0
    invalid = 0
    with HmdPoseShmReader(info) as reader:
        last_sequence = reader.latest_sequence()
        start = time.monotonic()
        last_print = 0.0
        while True:
            now = time.monotonic()
            elapsed = now - start
            if elapsed >= duration_sec:
                break
            latest = reader.latest_sequence()
            if latest < last_sequence:
                raise RuntimeError("HMD SHM sequence moved backwards; backend was restarted")
            if latest > last_sequence:
                first_available = max(1, latest - info.slot_count + 1)
                next_sequence = last_sequence + 1
                if next_sequence < first_available:
                    dropped += first_available - next_sequence
                    next_sequence = first_available
                for sequence in range(next_sequence, latest + 1):
                    sample = reader.read_sequence(sequence)
                    if sample is None:
                        invalid += 1
                        continue
                    if (sample.flags & HMD_FLAG_POSE_VALID) == 0:
                        invalid += 1
                        continue
                    samples.append(sample)
                last_sequence = latest
            if now - last_print >= 0.25:
                print(
                    f"\r  remaining={max(0.0, duration_sec - elapsed):4.1f}s "
                    f"samples={len(samples):4d}",
                    end="",
                    flush=True,
                )
                last_print = now
            time.sleep(max(0.0005, poll_ms / 1000.0))
    print()
    return samples, dropped, invalid


def collect_guided_samples(
    info: RegistryInfo,
    poll_ms: float,
    countdown: int,
    phase_scale: float,
) -> Tuple[List[TimedHmdSample], int, int, Tuple[GuidedPhase, ...]]:
    phases = make_guided_phases(phase_scale)
    duration_sec = phases[-1].end_sec
    if countdown:
        print(
            f"Guided recording starts in {countdown} seconds. "
            "Face forward in the neutral pose."
        )
        for remaining in range(countdown, 0, -1):
            print(f"  {remaining}", flush=True)
            time.sleep(1.0)

    samples: List[TimedHmdSample] = []
    dropped = 0
    invalid = 0
    last_phase_name = ""
    with HmdPoseShmReader(info) as reader:
        last_sequence = reader.latest_sequence()
        start = time.monotonic()
        last_print = 0.0
        while True:
            now = time.monotonic()
            elapsed = now - start
            if elapsed >= duration_sec:
                break
            phase = guided_phase_at(phases, elapsed)
            if phase.name != last_phase_name:
                print()
                print("\a" + "#" * 80)
                print(
                    f"[{phase.start_sec:04.1f}-{phase.end_sec:04.1f}s] "
                    f"{phase.instruction}"
                )
                print("#" * 80, flush=True)
                last_phase_name = phase.name

            latest = reader.latest_sequence()
            if latest < last_sequence:
                raise RuntimeError("HMD SHM sequence moved backwards; backend was restarted")
            if latest > last_sequence:
                first_available = max(1, latest - info.slot_count + 1)
                next_sequence = last_sequence + 1
                if next_sequence < first_available:
                    dropped += first_available - next_sequence
                    next_sequence = first_available
                for sequence in range(next_sequence, latest + 1):
                    sample = reader.read_sequence(sequence)
                    if sample is None or (sample.flags & HMD_FLAG_POSE_VALID) == 0:
                        invalid += 1
                        continue
                    samples.append(TimedHmdSample(elapsed_sec=elapsed, sample=sample))
                last_sequence = latest

            if now - last_print >= 0.25:
                print(
                    f"\r  phase={phase.name:14s} "
                    f"remaining={max(0.0, phase.end_sec - elapsed):4.1f}s "
                    f"samples={len(samples):5d}",
                    end="",
                    flush=True,
                )
                last_print = now
            time.sleep(max(0.0005, poll_ms / 1000.0))
    print("\n\a")
    return samples, dropped, invalid, phases


def calculate_result(
    samples: Sequence[HmdSample],
    basis: Quaternion,
    mode: str,
    multiply_order: str,
) -> CalibrationResult:
    source_quats = [sample.quaternion_xyzw for sample in samples]
    mean_source = q_average(source_quats)
    basis_quats = [q_apply_basis(basis, q) for q in source_quats]
    mean_basis = q_average(basis_quats)
    deviations = [q_angular_distance_deg(q, mean_basis) for q in basis_quats]

    if mode == "level":
        target = level_target_from_orientation(mean_basis)
    elif mode == "full-neutral":
        target = (0.0, 0.0, 0.0, 1.0)
    else:
        raise ValueError(f"unknown calibration mode: {mode}")

    if multiply_order in ("post", "local"):
        offset = q_mul(q_conj(mean_basis), target)
    elif multiply_order in ("pre", "world"):
        offset = q_mul(target, q_conj(mean_basis))
    else:
        raise ValueError(f"unknown multiply order: {multiply_order}")
    offset = q_normalize(offset, canonical=True)
    predicted = q_apply_offset(mean_basis, offset, multiply_order)

    angular_speeds = [
        v_norm(sample.angular_velocity)
        for sample in samples
        if (sample.flags & HMD_FLAG_ANGULAR_VELOCITY_VALID) != 0
    ]
    return CalibrationResult(
        mean_source_xyzw=q_normalize(mean_source, canonical=True),
        mean_basis_xyzw=q_normalize(mean_basis, canonical=True),
        target_xyzw=q_normalize(target, canonical=True),
        orientation_offset_xyzw=offset,
        predicted_output_xyzw=q_normalize(predicted, canonical=True),
        max_sample_deviation_deg=max(deviations) if deviations else 180.0,
        rms_sample_deviation_deg=(
            math.sqrt(statistics.fmean(value * value for value in deviations))
            if deviations
            else 180.0
        ),
        tilt_residual_deg=tilt_from_level_deg(predicted),
        full_residual_deg=q_angular_distance_deg(predicted, target),
        sample_count=len(samples),
        angular_speed_p95_rad_s=percentile(angular_speeds, 0.95),
    )


def verify_result(
    samples: Sequence[HmdSample],
    basis: Quaternion,
    offset: OffsetConfig,
    mode: str,
) -> CalibrationResult:
    if not offset.enabled:
        raise RuntimeError("verification requires an enabled orientation_offset")
    source_quats = [sample.quaternion_xyzw for sample in samples]
    mean_source = q_average(source_quats)
    basis_quats = [q_apply_basis(basis, q) for q in source_quats]
    mean_basis = q_average(basis_quats)
    output_quats = [
        q_apply_offset(q, offset.quaternion_xyzw, offset.multiply_order)
        for q in basis_quats
    ]
    mean_output = q_average(output_quats)
    deviations = [q_angular_distance_deg(q, mean_output) for q in output_quats]
    target = (
        level_target_from_orientation(mean_output)
        if mode == "level"
        else (0.0, 0.0, 0.0, 1.0)
    )
    angular_speeds = [
        v_norm(sample.angular_velocity)
        for sample in samples
        if (sample.flags & HMD_FLAG_ANGULAR_VELOCITY_VALID) != 0
    ]
    return CalibrationResult(
        mean_source_xyzw=q_normalize(mean_source, canonical=True),
        mean_basis_xyzw=q_normalize(mean_basis, canonical=True),
        target_xyzw=q_normalize(target, canonical=True),
        orientation_offset_xyzw=q_normalize(offset.quaternion_xyzw, canonical=True),
        predicted_output_xyzw=q_normalize(mean_output, canonical=True),
        max_sample_deviation_deg=max(deviations) if deviations else 180.0,
        rms_sample_deviation_deg=(
            math.sqrt(statistics.fmean(value * value for value in deviations))
            if deviations
            else 180.0
        ),
        tilt_residual_deg=tilt_from_level_deg(mean_output),
        full_residual_deg=(
            q_angular_distance_deg(mean_output, (0.0, 0.0, 0.0, 1.0))
            if mode == "full-neutral"
            else tilt_from_level_deg(mean_output)
        ),
        sample_count=len(samples),
        angular_speed_p95_rad_s=percentile(angular_speeds, 0.95),
    )


def phase_tail_samples(
    samples: Sequence[TimedHmdSample],
    phase: GuidedPhase,
    tail_fraction: float,
    end_guard_fraction: float,
) -> List[HmdSample]:
    fraction = clamp(tail_fraction, 0.05, 1.0)
    guard = clamp(end_guard_fraction, 0.0, 0.40)
    duration = phase.end_sec - phase.start_sec
    start = phase.end_sec - duration * fraction
    end = phase.end_sec - duration * guard
    if end <= start:
        raise RuntimeError(
            f"guided phase window is empty for {phase.name}; reduce end guard or increase tail fraction"
        )
    return [
        item.sample
        for item in samples
        if start <= item.elapsed_sec < end
    ]


def pose_window_stats(
    samples: Sequence[HmdSample],
    basis: Quaternion,
) -> Tuple[Quaternion, Quaternion, float, float, Optional[float]]:
    if not samples:
        raise RuntimeError("guided phase window contains no valid HMD samples")
    source_quats = [sample.quaternion_xyzw for sample in samples]
    basis_quats = [q_apply_basis(basis, q) for q in source_quats]
    mean_source = q_average(source_quats)
    mean_basis = q_average(basis_quats)
    deviations = [q_angular_distance_deg(q, mean_basis) for q in basis_quats]
    angular_speeds = [
        v_norm(sample.angular_velocity)
        for sample in samples
        if (sample.flags & HMD_FLAG_ANGULAR_VELOCITY_VALID) != 0
    ]
    return (
        mean_source,
        mean_basis,
        max(deviations) if deviations else 180.0,
        (
            math.sqrt(statistics.fmean(value * value for value in deviations))
            if deviations
            else 180.0
        ),
        percentile(angular_speeds, 0.95),
    )


def fit_guided_mount_offset(
    relative_by_motion: dict,
) -> Tuple[
    Quaternion,
    float,
    Tuple[float, float, float],
    dict,
    float,
]:
    desired_axes = {
        "pitch_up": (1.0, 0.0, 0.0),
        "yaw_right": (0.0, -1.0, 0.0),
        "roll_right": (0.0, 0.0, -1.0),
    }
    measured_axes = {}
    motion_angles_deg = {}
    for name in ("pitch_up", "yaw_right", "roll_right"):
        try:
            axis, angle_rad = q_to_axis_angle(relative_by_motion[name])
        except (KeyError, ValueError) as exc:
            raise RuntimeError(f"{name}: no usable one-way rotation: {exc}") from exc
        measured_axes[name] = axis
        motion_angles_deg[name] = math.degrees(angle_rad)

    # For a post-multiply sensor-to-HMD offset O:
    #   r_hmd = O^-1 * r_sensor * O
    # Therefore measured sensor axes equal R(O) times the desired HMD axes.
    # Matrix columns are measured directions for canonical +X, +Y, +Z.
    raw_matrix = matrix_from_columns(
        measured_axes["pitch_up"],
        v_neg(measured_axes["yaw_right"]),
        v_neg(measured_axes["roll_right"]),
    )
    determinant = matrix_determinant(raw_matrix)
    if determinant <= 0.20:
        raise RuntimeError(
            "guided motions form a reflected or nearly singular basis "
            f"(determinant={determinant:.4f}); check requested directions"
        )

    raw_columns = tuple(matrix_column(raw_matrix, index) for index in range(3))
    pair_angles = (
        v_angle_deg(raw_columns[0], raw_columns[1]),
        v_angle_deg(raw_columns[0], raw_columns[2]),
        v_angle_deg(raw_columns[1], raw_columns[2]),
    )
    fitted_matrix = closest_rotation(raw_matrix)
    fitted_columns = tuple(matrix_column(fitted_matrix, index) for index in range(3))
    fit_residuals = tuple(
        v_angle_deg(raw_columns[index], fitted_columns[index]) for index in range(3)
    )
    offset = matrix_to_q(fitted_matrix)

    details = {}
    for name, desired_axis in desired_axes.items():
        measured_axis = measured_axes[name]
        corrected_axis = q_rotate(q_conj(offset), measured_axis)
        details[name] = {
            "desired_axis": desired_axis,
            "measured_axis": measured_axis,
            "corrected_axis": v_normalize(corrected_axis),
            "motion_angle_deg": motion_angles_deg[name],
            "axis_error_deg": v_angle_deg(corrected_axis, desired_axis),
        }
    return (
        q_normalize(offset, canonical=True),
        determinant,
        pair_angles,
        details,
        max(fit_residuals),
    )


def calculate_guided_result(
    timed_samples: Sequence[TimedHmdSample],
    phases: Sequence[GuidedPhase],
    basis: Quaternion,
    applied_offset: Optional[OffsetConfig],
    neutral_tail_fraction: float,
    motion_tail_fraction: float,
    end_guard_fraction: float,
) -> GuidedCalibrationResult:
    phase_by_name = {phase.name: phase for phase in phases}
    motion_layout = (
        ("pitch_up", "neutral_start"),
        ("yaw_right", "neutral_pitch"),
        ("roll_right", "neutral_yaw"),
    )

    source_neutral_means: List[Quaternion] = []
    basis_neutral_means: List[Quaternion] = []
    selected_samples: List[HmdSample] = []
    neutral_window_max_deviations: List[float] = []
    neutral_window_rms_deviations: List[float] = []
    neutral_window_means: List[Quaternion] = []
    window_stats = {}
    relative_by_motion = {}

    for motion_name, neutral_name in motion_layout:
        neutral_samples = phase_tail_samples(
            timed_samples,
            phase_by_name[neutral_name],
            neutral_tail_fraction,
            end_guard_fraction,
        )
        endpoint_samples = phase_tail_samples(
            timed_samples,
            phase_by_name[motion_name],
            motion_tail_fraction,
            end_guard_fraction,
        )
        if len(neutral_samples) < 5 or len(endpoint_samples) < 5:
            raise RuntimeError(
                f"{motion_name}: too few samples in neutral/hold windows "
                f"({len(neutral_samples)}/{len(endpoint_samples)})"
            )
        neutral_stats = pose_window_stats(neutral_samples, basis)
        endpoint_stats = pose_window_stats(endpoint_samples, basis)
        source_neutral_means.append(neutral_stats[0])
        basis_neutral_means.append(neutral_stats[1])
        neutral_window_max_deviations.append(neutral_stats[2])
        neutral_window_rms_deviations.append(neutral_stats[3])
        neutral_window_means.append(neutral_stats[1])
        selected_samples.extend(neutral_samples)
        selected_samples.extend(endpoint_samples)
        relative_by_motion[motion_name] = q_mul(
            q_conj(neutral_stats[1]), endpoint_stats[1]
        )
        window_stats[motion_name] = {
            "neutral_count": len(neutral_samples),
            "endpoint_count": len(endpoint_samples),
            "neutral_max_deviation_deg": neutral_stats[2],
            "endpoint_max_deviation_deg": endpoint_stats[2],
            "neutral_angular_speed_p95_rad_s": neutral_stats[4],
            "endpoint_angular_speed_p95_rad_s": endpoint_stats[4],
        }

    final_neutral_samples = phase_tail_samples(
        timed_samples,
        phase_by_name["neutral_end"],
        neutral_tail_fraction,
        end_guard_fraction,
    )
    if len(final_neutral_samples) >= 5:
        final_stats = pose_window_stats(final_neutral_samples, basis)
        source_neutral_means.append(final_stats[0])
        basis_neutral_means.append(final_stats[1])
        neutral_window_max_deviations.append(final_stats[2])
        neutral_window_rms_deviations.append(final_stats[3])
        neutral_window_means.append(final_stats[1])
        selected_samples.extend(final_neutral_samples)

    (
        recommended_offset,
        determinant,
        pair_angles,
        fit_details,
        fit_residual_max,
    ) = fit_guided_mount_offset(relative_by_motion)

    if applied_offset is None:
        applied_q = recommended_offset
        applied_order = "post"
    else:
        if applied_offset.multiply_order not in ("post", "local"):
            raise RuntimeError("guided verification supports only a post/local offset")
        applied_q = q_normalize(applied_offset.quaternion_xyzw, canonical=True)
        applied_order = applied_offset.multiply_order

    mean_source = q_average(source_neutral_means)
    mean_basis = q_average(basis_neutral_means)
    predicted = q_apply_offset(mean_basis, applied_q, applied_order)
    target = level_target_from_orientation(predicted)

    neutral_output_means = [
        q_apply_offset(mean, applied_q, applied_order)
        for mean in neutral_window_means
    ]
    mean_neutral_output = q_average(neutral_output_means)
    neutral_tilts = [tilt_from_level_deg(q) for q in neutral_output_means]
    angular_speeds = [
        v_norm(sample.angular_velocity)
        for sample in selected_samples
        if (sample.flags & HMD_FLAG_ANGULAR_VELOCITY_VALID) != 0
    ]

    desired_axes = {
        "pitch_up": (1.0, 0.0, 0.0),
        "yaw_right": (0.0, -1.0, 0.0),
        "roll_right": (0.0, 0.0, -1.0),
    }
    motions = []
    for name in ("pitch_up", "yaw_right", "roll_right"):
        measured_axis, angle_rad = q_to_axis_angle(relative_by_motion[name])
        corrected_axis = q_rotate(q_conj(applied_q), measured_axis)
        stats = window_stats[name]
        motions.append(
            GuidedMotionResult(
                name=name,
                desired_axis=desired_axes[name],
                measured_axis=measured_axis,
                corrected_axis=v_normalize(corrected_axis),
                motion_angle_deg=math.degrees(angle_rad),
                axis_error_deg=v_angle_deg(corrected_axis, desired_axes[name]),
                candidate_fit_residual_deg=fit_details[name]["axis_error_deg"],
                neutral_sample_count=stats["neutral_count"],
                endpoint_sample_count=stats["endpoint_count"],
                neutral_max_deviation_deg=stats["neutral_max_deviation_deg"],
                endpoint_max_deviation_deg=stats["endpoint_max_deviation_deg"],
            )
        )

    calibration = CalibrationResult(
        mean_source_xyzw=q_normalize(mean_source, canonical=True),
        mean_basis_xyzw=q_normalize(mean_basis, canonical=True),
        target_xyzw=q_normalize(target, canonical=True),
        orientation_offset_xyzw=q_normalize(applied_q, canonical=True),
        predicted_output_xyzw=q_normalize(predicted, canonical=True),
        max_sample_deviation_deg=(
            max(neutral_window_max_deviations)
            if neutral_window_max_deviations
            else 180.0
        ),
        rms_sample_deviation_deg=(
            max(neutral_window_rms_deviations)
            if neutral_window_rms_deviations
            else 180.0
        ),
        tilt_residual_deg=max(neutral_tilts) if neutral_tilts else 180.0,
        full_residual_deg=max(neutral_tilts) if neutral_tilts else 180.0,
        sample_count=len(selected_samples),
        angular_speed_p95_rad_s=percentile(angular_speeds, 0.95),
    )
    return GuidedCalibrationResult(
        calibration=calibration,
        recommended_offset_xyzw=recommended_offset,
        applied_offset_xyzw=q_normalize(applied_q, canonical=True),
        offset_difference_deg=q_angular_distance_deg(recommended_offset, applied_q),
        motions=tuple(motions),
        raw_basis_determinant=determinant,
        measured_pair_angles_deg=pair_angles,
        candidate_fit_residual_max_deg=fit_residual_max,
    )


def guided_report_dict(result: GuidedCalibrationResult) -> dict:
    return {
        "recommended_orientation_offset_xyzw": list(result.recommended_offset_xyzw),
        "applied_orientation_offset_xyzw": list(result.applied_offset_xyzw),
        "applied_vs_recommended_difference_deg": result.offset_difference_deg,
        "raw_motion_basis_determinant": result.raw_basis_determinant,
        "measured_pair_angles_deg": {
            "xy": result.measured_pair_angles_deg[0],
            "xz": result.measured_pair_angles_deg[1],
            "yz": result.measured_pair_angles_deg[2],
        },
        "candidate_fit_residual_max_deg": result.candidate_fit_residual_max_deg,
        "motions": {
            motion.name: {
                "desired_axis": list(motion.desired_axis),
                "measured_axis": list(motion.measured_axis),
                "corrected_axis": list(motion.corrected_axis),
                "motion_angle_deg": motion.motion_angle_deg,
                "axis_error_deg": motion.axis_error_deg,
                "candidate_fit_residual_deg": motion.candidate_fit_residual_deg,
                "neutral_sample_count": motion.neutral_sample_count,
                "endpoint_sample_count": motion.endpoint_sample_count,
                "neutral_max_deviation_deg": motion.neutral_max_deviation_deg,
                "endpoint_max_deviation_deg": motion.endpoint_max_deviation_deg,
            }
            for motion in result.motions
        },
    }


def offset_json_block(offset: Quaternion) -> dict:
    """Return the only supported runtime correction: a world/pre offset."""

    return {
        "enabled": True,
        "multiply_order": "pre",
        "quaternion_xyzw": [
            round(value, 12) for value in q_normalize(offset, canonical=True)
        ],
    }


def write_config(path: Path, data: dict, block: dict) -> Path:
    streams = data.setdefault("streams", {})
    if not isinstance(streams, dict):
        raise RuntimeError("transform config field 'streams' is not an object")
    stream = streams.get("hmd")
    if not isinstance(stream, dict):
        raise RuntimeError("transform config has no stream object: hmd")
    stream["orientation_offset"] = dict(block)

    timestamp = datetime.now().strftime("%Y%m%d-%H%M%S")
    backup = path.with_name(path.name + f".bak.{timestamp}")
    suffix = 1
    while backup.exists():
        backup = path.with_name(path.name + f".bak.{timestamp}.{suffix}")
        suffix += 1
    shutil.copy2(path, backup)

    fd, temp_name = tempfile.mkstemp(prefix=path.name + ".tmp.", dir=str(path.parent))
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as file:
            json.dump(data, file, ensure_ascii=False, indent=2)
            file.write("\n")
            file.flush()
            os.fsync(file.fileno())
        os.replace(temp_name, path)
    except Exception:
        try:
            os.unlink(temp_name)
        except OSError:
            pass
        raise
    return backup


def compact_q(q: Quaternion) -> str:
    return "[" + ", ".join(f"{value:+.9f}" for value in q) + "]"


def compact_v3(v: Vector3) -> str:
    return "[" + ", ".join(f"{value:+.3f}" for value in v) + "]"



def run_self_test() -> int:
    identity = (0.0, 0.0, 0.0, 1.0)

    # A source pose looking 90 degrees upward must be corrected by a world/pre
    # -90 degree X rotation while preserving local motion axes.
    source = q_from_axis_angle((1.0, 0.0, 0.0), math.radians(90.0))
    result = calculate_result(
        [
            HmdSample(
                sequence=index + 1,
                timestamp_ns=index + 1,
                source_timestamp_ns=index + 1,
                quaternion_xyzw=source,
                angular_velocity=(0.0, 0.0, 0.0),
                tracking_status=0,
                flags=HMD_FLAG_POSE_VALID | HMD_FLAG_ANGULAR_VELOCITY_VALID,
                confidence=1.0,
            )
            for index in range(100)
        ],
        identity,
        "level",
        "pre",
    )
    expected = q_from_axis_angle((1.0, 0.0, 0.0), math.radians(-90.0))
    if q_angular_distance_deg(result.orientation_offset_xyzw, expected) > 1e-6:
        raise AssertionError("level calibration did not recover the world/pre offset")
    if tilt_from_level_deg(result.predicted_output_xyzw) > 1e-6:
        raise AssertionError("level calibration output is not level")

    arbitrary = q_from_euler_basis_xyz(0.3, -0.4, 0.2)
    full_offset = q_conj(arbitrary)
    if q_angular_distance_deg(q_mul(full_offset, arbitrary), identity) > 1e-6:
        raise AssertionError("full-neutral pre offset check failed")

    block = offset_json_block(expected)
    if block["multiply_order"] != "pre":
        raise AssertionError("generated offset is not pre/world")

    with tempfile.TemporaryDirectory() as temp_dir:
        config_path = Path(temp_dir) / "config.json"
        config = {"enabled": True, "streams": {"hmd": {}, "hmd_3dof": {}}}
        config_path.write_text(json.dumps(config), encoding="utf-8")
        backup = write_config(config_path, config, block)
        written = json.loads(config_path.read_text(encoding="utf-8"))
        if not backup.exists():
            raise AssertionError("config backup was not created")
        if written["streams"]["hmd"].get("orientation_offset") != block:
            raise AssertionError("hmd orientation_offset was not written")
        if "orientation_offset" in written["streams"]["hmd_3dof"]:
            raise AssertionError("hmd_3dof must not be modified")

    print("Self-test: OK")
    return 0


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Calibrate a world/pre HMD orientation_offset from one still neutral pose"
        )
    )
    parser.add_argument(
        "--registry",
        default=os.environ.get("TRACKING_REGISTRY", "/tmp/tracking_streams.json"),
        help="backend tracking SHM registry",
    )
    parser.add_argument(
        "--stream",
        default=os.environ.get("HMD_STREAM", "hmd_pose"),
        help="backend HMD_POSE_F64_LE stream ID",
    )
    parser.add_argument(
        "--config",
        default=os.environ.get("TRACKING_TRANSFORM_CONFIG", ""),
        help=(
            "optional xr_runtime_adapter transform JSON; without it the tool "
            "runs standalone and only prints the calculated quaternion"
        ),
    )
    parser.add_argument(
        "--mode",
        choices=("level", "full-neutral"),
        default="level",
        help="level preserves horizontal heading; full-neutral also resets heading",
    )
    parser.add_argument(
        "--duration",
        type=float,
        default=5.0,
        help="still neutral-pose capture duration in seconds",
    )
    parser.add_argument("--countdown", type=int, default=3, help="countdown before capture")
    parser.add_argument("--poll-ms", type=float, default=2.0, help="SHM poll interval")
    parser.add_argument("--min-samples", type=int, default=80, help="minimum accepted pose samples")
    parser.add_argument(
        "--max-deviation-deg",
        type=float,
        default=3.0,
        help="maximum allowed sample deviation from the mean pose",
    )
    parser.add_argument(
        "--max-angular-speed-rad-s",
        type=float,
        default=0.20,
        help="maximum allowed 95th-percentile angular speed when velocity is valid",
    )
    parser.add_argument("--write", action="store_true", help="atomically write the result to --config")
    parser.add_argument(
        "--replace-existing-offset",
        action="store_true",
        help="allow calibration when source orientation_offset is already enabled",
    )
    parser.add_argument(
        "--verify",
        action="store_true",
        help="verify the existing pre orientation_offset instead of calculating a new one",
    )
    parser.add_argument(
        "--output",
        default="/tmp/hmd_orientation_offset_calibration.json",
        help="JSON report path",
    )
    parser.add_argument("--self-test", action="store_true", help="run deterministic self-tests")
    return parser.parse_args()


def orientation_transform_enabled(config: dict, stream_name: str) -> bool:
    try:
        stream = config["streams"][stream_name]
    except (KeyError, TypeError) as exc:
        raise RuntimeError(f"transform config has no streams.{stream_name}") from exc
    block = stream.get("orientation_transform", {})
    return isinstance(block, dict) and bool(block.get("enabled", False))


def main() -> int:
    args = parse_args()
    if args.self_test:
        return run_self_test()
    if args.duration <= 0.0 or args.poll_ms <= 0.0:
        raise RuntimeError("--duration and --poll-ms must be greater than zero")
    if args.countdown < 0 or args.min_samples <= 0:
        raise RuntimeError("--countdown must be >= 0 and --min-samples must be > 0")
    if args.write and args.verify:
        raise RuntimeError("--write and --verify cannot be used together")
    if not args.config and args.write:
        raise RuntimeError("--write requires --config or TRACKING_TRANSFORM_CONFIG")
    if not args.config and args.verify:
        raise RuntimeError("--verify requires --config or TRACKING_TRANSFORM_CONFIG")

    standalone = not bool(args.config)
    config_path: Optional[Path] = None
    config: Optional[dict] = None
    existing_offset: Optional[OffsetConfig] = None

    if not standalone:
        config_path = Path(args.config).expanduser().resolve()
        try:
            config = json.loads(config_path.read_text(encoding="utf-8"))
        except FileNotFoundError as exc:
            raise RuntimeError(f"transform config not found: {config_path}") from exc
        except json.JSONDecodeError as exc:
            raise RuntimeError(
                f"invalid transform config JSON {config_path}: {exc}"
            ) from exc

        if orientation_transform_enabled(config, "hmd"):
            raise RuntimeError(
                "streams.hmd.orientation_transform is enabled; "
                "axis/mount normalization must be done in capture_service_cpp. "
                "Disable the runtime orientation_transform before neutral calibration."
            )

        existing_offset = load_offset(config, "hmd")
        if args.verify and existing_offset.multiply_order not in ("pre", "world"):
            raise RuntimeError(
                f"verification requires a pre/world orientation_offset; got "
                f"{existing_offset.multiply_order!r}"
            )
        if (
            not args.verify
            and existing_offset.enabled
            and not args.replace_existing_offset
        ):
            raise RuntimeError(
                "streams.hmd.orientation_offset is already enabled; "
                "disable it first or use --replace-existing-offset"
            )

    registry_path = Path(args.registry).expanduser()
    info = read_registry(registry_path, args.stream)
    identity_basis: Quaternion = (0.0, 0.0, 0.0, 1.0)

    print("=" * 80)
    print("XR Gate neutral HMD orientation calibration")
    print(f"registry:          {info.registry_path}")
    print(f"stream:            {info.stream_id} ({info.format_name})")
    print(f"frame:             {info.frame_id}")
    print(
        f"config:            {config_path}"
        if config_path is not None
        else "config:            <standalone; not used>"
    )
    if config_path is not None:
        print("config target:     streams.hmd.orientation_offset")
    print(f"mode:              {args.mode}")
    print("multiply order:    pre (world)")
    print()
    print("Look straight ahead in the intended neutral HMD pose and hold still.")
    if args.mode == "level":
        print("The current horizontal heading is preserved; tilt and roll are removed.")
    else:
        print("The complete current orientation, including heading, becomes identity.")
    print("IMU axes/signs are assumed to be normalized by capture_service_cpp.")
    print("=" * 80)

    samples, dropped, invalid = collect_samples(
        info, args.duration, args.poll_ms, args.countdown
    )
    if len(samples) < args.min_samples:
        raise RuntimeError(
            f"too few valid HMD samples: {len(samples)}; need at least {args.min_samples}"
        )

    if args.verify:
        if existing_offset is None:
            raise RuntimeError("internal error: verification offset was not loaded")
        result = verify_result(samples, identity_basis, existing_offset, args.mode)
    else:
        result = calculate_result(samples, identity_basis, args.mode, "pre")

    problems: List[str] = []
    if result.max_sample_deviation_deg > args.max_deviation_deg:
        problems.append(
            f"pose moved too much: max deviation {result.max_sample_deviation_deg:.2f} deg "
            f"> {args.max_deviation_deg:.2f} deg"
        )
    if (
        result.angular_speed_p95_rad_s is not None
        and result.angular_speed_p95_rad_s > args.max_angular_speed_rad_s
    ):
        problems.append(
            f"neutral pose was not still: angular-speed p95 "
            f"{result.angular_speed_p95_rad_s:.3f} rad/s "
            f"> {args.max_angular_speed_rad_s:.3f} rad/s"
        )

    block = offset_json_block(result.orientation_offset_xyzw)
    euler_debug = q_to_euler_xyz_deg(result.orientation_offset_xyzw)

    report = {
        "format": REPORT_FORMAT,
        "success": not problems,
        "verification": bool(args.verify),
        "registry": str(info.registry_path),
        "stream": info.stream_id,
        "frame_id": info.frame_id,
        "config": str(config_path) if config_path is not None else None,
        "source_transform": "hmd" if config_path is not None else None,
        "targets": ["hmd"] if config_path is not None else [],
        "mode": args.mode,
        "sample_count": result.sample_count,
        "dropped_sequences": dropped,
        "invalid_samples": invalid,
        "max_sample_deviation_deg": result.max_sample_deviation_deg,
        "rms_sample_deviation_deg": result.rms_sample_deviation_deg,
        "angular_speed_p95_rad_s": result.angular_speed_p95_rad_s,
        "mean_source_quaternion_xyzw": list(result.mean_source_xyzw),
        "target_quaternion_xyzw": list(result.target_xyzw),
        "orientation_offset": block,
        "orientation_offset_euler_xyz_deg_debug": {
            "rx_deg": euler_debug[0],
            "ry_deg": euler_debug[1],
            "rz_deg": euler_debug[2],
        },
        "predicted_output_quaternion_xyzw": list(result.predicted_output_xyzw),
        "predicted_tilt_residual_deg": result.tilt_residual_deg,
        "predicted_full_residual_deg": result.full_residual_deg,
        "problems": problems,
    }
    output_path: Optional[Path] = None
    if not standalone:
        output_path = Path(args.output).expanduser()
        output_path.parent.mkdir(parents=True, exist_ok=True)
        output_path.write_text(
            json.dumps(report, ensure_ascii=False, indent=2) + "\n",
            encoding="utf-8",
        )

    print("=" * 80)
    print("RESULT")
    print(f"samples:                    {result.sample_count}")
    print(f"dropped/invalid:            {dropped}/{invalid}")
    print(f"max neutral deviation:      {result.max_sample_deviation_deg:.3f} deg")
    print(f"rms neutral deviation:      {result.rms_sample_deviation_deg:.3f} deg")
    print(
        "neutral angular speed p95:  "
        + (
            f"{result.angular_speed_p95_rad_s:.4f} rad/s"
            if result.angular_speed_p95_rad_s is not None
            else "<not available>"
        )
    )
    print(f"mean source xyzw:           {compact_q(result.mean_source_xyzw)}")
    print(f"pre orientation offset:     {compact_q(result.orientation_offset_xyzw)}")
    print(f"offset Euler debug xyz deg: {compact_v3(euler_debug)}")
    print(f"predicted neutral tilt:     {result.tilt_residual_deg:.6f} deg")
    if problems:
        print("\nCALIBRATION NOT RELIABLE - config was not changed.")
        for problem in problems:
            print(f"  - {problem}")
        if output_path is not None:
            print(f"report: {output_path}")
        return 2

    if standalone:
        print("\nCALIBRATION PASSED")
        print()
        print(json.dumps({"orientation_offset": block}, indent=2))
    elif args.verify:
        print("\nVERIFICATION PASSED")
    elif args.write:
        if config_path is None or config is None:
            raise RuntimeError("internal error: write requested without config")
        backup = write_config(config_path, config, block)
        print("\nCONFIG UPDATED")
        print(f"config: {config_path}")
        print(f"backup: {backup}")
        print("target: streams.hmd.orientation_offset")
        print("Restart xr_runtime_adapter to apply the new pre offset.")
    else:
        print("\nCALIBRATION PASSED - use --write to update the config atomically.")
    if output_path is not None:
        print(f"report: {output_path}")
    print("=" * 80)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except KeyboardInterrupt:
        print("\nCalibration interrupted.", file=sys.stderr)
        raise SystemExit(130)
    except Exception as exc:
        print(f"[ERROR] {exc}", file=sys.stderr)
        raise SystemExit(1)
