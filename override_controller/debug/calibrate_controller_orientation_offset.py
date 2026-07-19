#!/usr/bin/env python3
"""Calibrate a fixed per-controller IMU orientation offset.

The tool reads one side of override_controller's CONTROLLER_INPUT_V3 SHM stream.
`orientation_transform` must already normalize IMU axes and signs. Hold the
physical controller in the desired neutral grip pose, keep it still, and the
tool computes the post/local quaternion that makes that pose identity:

    q_output = q_transformed * q_offset

Without --config it only prints a ready-to-copy devices[].orientation_offset
JSON block. With --config and --write it updates the unique devices[] entry whose
imu_side matches --side and creates a timestamped backup.

If the selected device already has an enabled offset and the running
`override_controller` loaded the same config, pass --replace-existing-offset.
The tool removes that existing offset from the observed stream before fitting the
replacement. Otherwise disable the old offset and restart override_controller.

#LEFT CONTROLLER
python3 debug/calibrate_controller_orientation_offset.py \
  --side left \
  --registry /tmp/tracking_streams.json \
  --stream controller_input

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

RING_HEADER_STRUCT = struct.Struct("<8s7IQ")
SLOT_HEADER_STRUCT = struct.Struct("<QQQQII")
FRAME_HEADER_SIZE = 56
V3_FRAME_SIZE = 1432
V3_SIDE_SIZE = 688
V3_IMU_OFF = 400
IMU_SAMPLES_OFF = 80
IMU_SAMPLE_SIZE = 48
SIDE_LEFT = 0
SIDE_RIGHT = 1
IMU_STATUS_ACTIVE = 3
IMU_STATUS_STALE = 4
IMU_ORIENTATION_VALID = 1 << 3
IMU_GYROSCOPE_VALID = 1 << 0

Quaternion = Tuple[float, float, float, float]
Vector3 = Tuple[float, float, float]


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
    format_name: str


@dataclass(frozen=True)
class OffsetConfig:
    enabled: bool
    multiply_order: str
    quaternion_xyzw: Quaternion


@dataclass(frozen=True)
class Sample:
    orientation_timestamp_ns: int
    orientation_xyzw: Quaternion
    angular_velocity_rad_s: Optional[Vector3]
    imu_status: int


def clamp(value: float, low: float, high: float) -> float:
    return max(low, min(high, value))


def finite(values: Iterable[float]) -> bool:
    return all(math.isfinite(value) for value in values)


def q_normalize(q: Quaternion, canonical: bool = False) -> Quaternion:
    norm = math.sqrt(sum(value * value for value in q))
    if not math.isfinite(norm) or norm <= 1.0e-12:
        raise ValueError("cannot normalize a zero/non-finite quaternion")
    out = tuple(value / norm for value in q)
    if canonical and out[3] < 0.0:
        out = tuple(-value for value in out)
    return out  # type: ignore[return-value]


def q_conj(q: Quaternion) -> Quaternion:
    q = q_normalize(q)
    return (-q[0], -q[1], -q[2], q[3])


def q_mul(a: Quaternion, b: Quaternion) -> Quaternion:
    ax, ay, az, aw = a
    bx, by, bz, bw = b
    return q_normalize((
        aw * bx + ax * bw + ay * bz - az * by,
        aw * by - ax * bz + ay * bw + az * bx,
        aw * bz + ax * by - ay * bx + az * bw,
        aw * bw - ax * bx - ay * by - az * bz,
    ))


def q_dot(a: Quaternion, b: Quaternion) -> float:
    return sum(left * right for left, right in zip(a, b))


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


def q_distance_deg(a: Quaternion, b: Quaternion) -> float:
    dot = clamp(abs(q_dot(q_normalize(a), q_normalize(b))), 0.0, 1.0)
    return math.degrees(2.0 * math.acos(dot))


def remove_offset(observed: Quaternion, offset: OffsetConfig) -> Quaternion:
    if not offset.enabled:
        return q_normalize(observed)
    inverse = q_conj(offset.quaternion_xyzw)
    if offset.multiply_order in ("pre", "world"):
        return q_mul(inverse, observed)
    return q_mul(observed, inverse)


def apply_offset(base: Quaternion, offset: Quaternion, order: str) -> Quaternion:
    if order in ("pre", "world"):
        return q_mul(offset, base)
    return q_mul(base, offset)


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


def vector_norm(v: Vector3) -> float:
    return math.sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2])


def read_registry(path: Path, stream_id: str) -> RegistryInfo:
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except FileNotFoundError as exc:
        raise RuntimeError(
            f"controller registry not found: {path}; start override_controller first"
        ) from exc
    except json.JSONDecodeError as exc:
        raise RuntimeError(f"invalid registry JSON {path}: {exc}") from exc
    streams = data.get("streams")
    if not isinstance(streams, dict) or stream_id not in streams:
        available = ", ".join(sorted(streams)) if isinstance(streams, dict) else "<none>"
        raise RuntimeError(f"stream {stream_id!r} not found; available: {available}")
    entry = streams[stream_id]
    if not isinstance(entry, dict):
        raise RuntimeError(f"registry entry {stream_id!r} is not an object")
    payload_size = int(entry.get("payload_size", V3_FRAME_SIZE))
    slot_header_size = int(entry.get("slot_header_size", 128))
    slot_stride = int(entry.get("slot_stride", slot_header_size + payload_size))
    info = RegistryInfo(
        registry_path=path,
        stream_id=stream_id,
        shm_name=str(entry.get("shm_name", stream_id)),
        header_size=int(entry.get("header_size", 4096)),
        slot_count=int(entry.get("slot_count", 1024)),
        slot_stride=slot_stride,
        slot_header_size=slot_header_size,
        payload_size=payload_size,
        format_name=str(entry.get("format_name", entry.get("format", ""))),
    )
    if info.payload_size < V3_FRAME_SIZE:
        raise RuntimeError(
            f"controller payload is too small: {info.payload_size} < {V3_FRAME_SIZE}"
        )
    if info.slot_count <= 0 or info.header_size <= 0:
        raise RuntimeError("invalid controller SHM dimensions")
    return info


def shm_path(name: str) -> Path:
    return Path("/dev/shm") / name.lstrip("/")


class ControllerInputReader:
    def __init__(self, info: RegistryInfo):
        self.info = info
        self.path = shm_path(info.shm_name)
        if not self.path.exists():
            raise RuntimeError(f"SHM not found: {self.path}; start override_controller first")
        expected = info.header_size + info.slot_count * info.slot_stride
        if self.path.stat().st_size < expected:
            raise RuntimeError(f"SHM is too small: {self.path.stat().st_size} < {expected}")
        self._file = self.path.open("rb")
        self._mm = mmap.mmap(self._file.fileno(), 0, access=mmap.ACCESS_READ)

    def close(self) -> None:
        self._mm.close()
        self._file.close()

    def __enter__(self) -> "ControllerInputReader":
        return self

    def __exit__(self, exc_type, exc, traceback) -> None:
        self.close()

    def latest_sequence(self) -> int:
        latest = 0
        if len(self._mm) >= RING_HEADER_STRUCT.size:
            try:
                values = RING_HEADER_STRUCT.unpack_from(self._mm, 0)
                latest = int(values[-1])
            except Exception:
                latest = 0
        if len(self._mm) >= 48:
            legacy = struct.unpack_from("<Q", self._mm, 40)[0]
            if legacy and legacy < (1 << 32) and (latest == 0 or latest > (1 << 32)):
                latest = int(legacy)
        return latest

    def read_sample(self, sequence: int, side_index: int) -> Optional[Sample]:
        latest = self.latest_sequence()
        if sequence <= 0 or latest == 0 or sequence > latest or latest - sequence >= self.info.slot_count:
            return None
        slot_index = (sequence - 1) % self.info.slot_count
        slot_offset = self.info.header_size + slot_index * self.info.slot_stride
        payload_offset = slot_offset + self.info.slot_header_size
        if payload_offset + V3_FRAME_SIZE > len(self._mm):
            return None
        header1 = SLOT_HEADER_STRUCT.unpack_from(self._mm, slot_offset)
        seq_begin, seq_end, _ts, _source_ts, payload_size, _flags = header1
        if seq_begin != seq_end or seq_begin == 0 or (seq_begin & 1):
            return None
        actual_sequence = seq_end // 2
        if actual_sequence != sequence or payload_size < V3_FRAME_SIZE:
            return None
        payload = bytes(self._mm[payload_offset:payload_offset + V3_FRAME_SIZE])
        header2 = SLOT_HEADER_STRUCT.unpack_from(self._mm, slot_offset)
        if header1[:2] != header2[:2]:
            return None
        version, size_bytes = struct.unpack_from("<II", payload, 0)
        if version != 3 or size_bytes != V3_FRAME_SIZE:
            return None
        side_base = FRAME_HEADER_SIZE + side_index * V3_SIDE_SIZE
        imu_base = side_base + V3_IMU_OFF
        status, _caps, data_flags, sample_count = struct.unpack_from("<IIII", payload, imu_base)
        if (data_flags & IMU_ORIENTATION_VALID) == 0:
            return None
        _imu_seq, _latest_ts, _latest_ticks, orientation_ts = struct.unpack_from(
            "<QQQQ", payload, imu_base + 16
        )
        q = struct.unpack_from("<4f", payload, imu_base + 48)
        if not finite(q):
            return None
        try:
            orientation = q_normalize(tuple(float(v) for v in q))  # type: ignore[arg-type]
        except ValueError:
            return None
        angular: Optional[Vector3] = None
        count = min(int(sample_count), 4)
        if count > 0 and (data_flags & IMU_GYROSCOPE_VALID) != 0:
            sample_base = imu_base + IMU_SAMPLES_OFF + (count - 1) * IMU_SAMPLE_SIZE
            values = struct.unpack_from("<3f", payload, sample_base + 16)
            if finite(values):
                angular = tuple(float(v) for v in values)  # type: ignore[assignment]
        return Sample(int(orientation_ts), orientation, angular, int(status))


def collect_samples(
    info: RegistryInfo,
    side_index: int,
    duration_sec: float,
    poll_ms: float,
    countdown: int,
) -> Tuple[List[Sample], int, int]:
    if countdown:
        print(f"Recording starts in {countdown} seconds. Hold the controller in the desired neutral pose.")
        for remaining in range(countdown, 0, -1):
            print(f"  {remaining}", flush=True)
            time.sleep(1.0)
    samples: List[Sample] = []
    dropped = 0
    invalid = 0
    last_orientation_ts = 0
    with ControllerInputReader(info) as reader:
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
                raise RuntimeError("controller SHM sequence moved backwards; override_controller restarted")
            if latest > last_sequence:
                first_available = max(1, latest - info.slot_count + 1)
                next_sequence = last_sequence + 1
                if next_sequence < first_available:
                    dropped += first_available - next_sequence
                    next_sequence = first_available
                for sequence in range(next_sequence, latest + 1):
                    sample = reader.read_sample(sequence, side_index)
                    if sample is None or sample.imu_status not in (IMU_STATUS_ACTIVE, IMU_STATUS_STALE):
                        invalid += 1
                        continue
                    if sample.orientation_timestamp_ns == 0:
                        invalid += 1
                        continue
                    if sample.orientation_timestamp_ns == last_orientation_ts:
                        continue
                    last_orientation_ts = sample.orientation_timestamp_ns
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


def offset_from_json(block: object) -> OffsetConfig:
    if not isinstance(block, dict):
        return OffsetConfig(False, "post", (0.0, 0.0, 0.0, 1.0))
    enabled = bool(block.get("enabled", False))
    order = str(block.get("multiply_order", "post"))
    if order not in ("post", "local", "pre", "world"):
        raise RuntimeError(f"invalid existing orientation_offset multiply_order: {order}")
    raw = block.get("quaternion_xyzw", [0.0, 0.0, 0.0, 1.0])
    if not isinstance(raw, list) or len(raw) != 4:
        raise RuntimeError("existing orientation_offset.quaternion_xyzw must contain four values")
    q = q_normalize(tuple(float(v) for v in raw))  # type: ignore[arg-type]
    return OffsetConfig(enabled, order, q)


def load_config_device(path: Path, side: str) -> Tuple[dict, dict, OffsetConfig]:
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except FileNotFoundError as exc:
        raise RuntimeError(f"override_controller config not found: {path}") from exc
    except json.JSONDecodeError as exc:
        raise RuntimeError(f"invalid config JSON {path}: {exc}") from exc
    devices = data.get("devices")
    if not isinstance(devices, list):
        raise RuntimeError("config has no devices[] array")
    matches = [device for device in devices if isinstance(device, dict) and device.get("imu_side") == side]
    if len(matches) != 1:
        raise RuntimeError(
            f"expected exactly one devices[] entry with imu_side={side!r}; found {len(matches)}"
        )
    device = matches[0]
    return data, device, offset_from_json(device.get("orientation_offset"))


def make_block(offset: Quaternion) -> dict:
    values = []
    for value in q_normalize(offset, canonical=True):
        rounded = round(value, 12)
        values.append(0.0 if abs(rounded) < 0.5e-12 else rounded)
    return {
        "enabled": True,
        "multiply_order": "post",
        "quaternion_xyzw": values,
    }


def write_config(path: Path, data: dict, device: dict, block: dict) -> Path:
    device["orientation_offset"] = dict(block)
    data["version"] = max(5, int(data.get("version", 0) or 0))
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


def run_self_test() -> int:
    q = q_normalize((0.2, -0.3, 0.4, 0.8))
    offset = q_conj(q)
    result = apply_offset(q, offset, "post")
    if q_distance_deg(result, (0.0, 0.0, 0.0, 1.0)) > 1.0e-6:
        raise AssertionError("post neutral offset did not produce identity")
    existing = OffsetConfig(True, "post", q_normalize((0.1, 0.2, 0.3, 0.9)))
    observed = apply_offset(q, existing.quaternion_xyzw, existing.multiply_order)
    restored = remove_offset(observed, existing)
    if q_distance_deg(restored, q) > 1.0e-6:
        raise AssertionError("existing post offset removal failed")
    print("Self-test: OK")
    return 0


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Calibrate devices[].orientation_offset for one controller IMU side"
    )
    parser.add_argument("--side", choices=("left", "right"), default="")
    parser.add_argument(
        "--registry",
        default=os.environ.get("CONTROLLER_INPUT_REGISTRY", "/tmp/tracking_streams.json"),
    )
    parser.add_argument(
        "--stream",
        default=os.environ.get("CONTROLLER_INPUT_STREAM", "controller_input"),
    )
    parser.add_argument("--config", default="", help="optional override_controller config JSON")
    parser.add_argument("--duration", type=float, default=5.0)
    parser.add_argument("--countdown", type=int, default=3)
    parser.add_argument("--poll-ms", type=float, default=2.0)
    parser.add_argument("--min-samples", type=int, default=80)
    parser.add_argument("--max-deviation-deg", type=float, default=3.0)
    parser.add_argument("--max-angular-speed-rad-s", type=float, default=0.20)
    parser.add_argument("--replace-existing-offset", action="store_true")
    parser.add_argument("--write", action="store_true")
    parser.add_argument("--self-test", action="store_true")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if args.self_test:
        return run_self_test()
    if not args.side:
        raise RuntimeError("--side left or --side right is required")
    if args.duration <= 0.0 or args.poll_ms <= 0.0:
        raise RuntimeError("--duration and --poll-ms must be greater than zero")
    if args.countdown < 0 or args.min_samples <= 0:
        raise RuntimeError("--countdown must be >= 0 and --min-samples must be > 0")
    if args.write and not args.config:
        raise RuntimeError("--write requires --config")

    config_path: Optional[Path] = None
    config: Optional[dict] = None
    config_device: Optional[dict] = None
    existing = OffsetConfig(False, "post", (0.0, 0.0, 0.0, 1.0))
    if args.config:
        config_path = Path(args.config).expanduser().resolve()
        config, config_device, existing = load_config_device(config_path, args.side)
        if existing.enabled and not args.replace_existing_offset:
            raise RuntimeError(
                f"the {args.side} device orientation_offset is already enabled; disable it and "
                "restart override_controller, or use --replace-existing-offset with the same running config"
            )

    info = read_registry(Path(args.registry).expanduser(), args.stream)
    side_index = SIDE_LEFT if args.side == "left" else SIDE_RIGHT
    print("=" * 80)
    print("XR Gate controller IMU orientation-offset calibration")
    print(f"registry:          {info.registry_path}")
    print(f"stream:            {info.stream_id} ({info.format_name or 'CONTROLLER_INPUT_V3'})")
    print(f"side:              {args.side}")
    print(f"config:            {config_path if config_path else '<standalone; not used>'}")
    print("multiply order:    post (local/controller)")
    print()
    print("Hold the controller in the desired neutral grip pose and keep it still.")
    print("The current transformed controller orientation will become identity.")
    if not config_path:
        print("IMPORTANT: any existing devices[].orientation_offset must be disabled.")
    elif existing.enabled:
        print("The configured existing offset will be removed before fitting its replacement.")
    print("=" * 80)

    samples, dropped, invalid = collect_samples(
        info, side_index, args.duration, args.poll_ms, args.countdown
    )
    if len(samples) < args.min_samples:
        raise RuntimeError(
            f"too few distinct orientation samples: {len(samples)}; need {args.min_samples}"
        )
    raw_quats = [remove_offset(sample.orientation_xyzw, existing) for sample in samples]
    mean = q_average(raw_quats)
    deviations = [q_distance_deg(q, mean) for q in raw_quats]
    offset = q_conj(mean)
    predicted = apply_offset(mean, offset, "post")
    speeds = [
        vector_norm(sample.angular_velocity_rad_s)
        for sample in samples
        if sample.angular_velocity_rad_s is not None
    ]
    p95 = percentile(speeds, 0.95)
    max_deviation = max(deviations)
    rms_deviation = math.sqrt(statistics.fmean(value * value for value in deviations))

    problems: List[str] = []
    if max_deviation > args.max_deviation_deg:
        problems.append(
            f"controller moved too much: max deviation {max_deviation:.2f} deg > {args.max_deviation_deg:.2f} deg"
        )
    if p95 is not None and p95 > args.max_angular_speed_rad_s:
        problems.append(
            f"controller was not still: angular-speed p95 {p95:.3f} rad/s > {args.max_angular_speed_rad_s:.3f} rad/s"
        )

    block = make_block(offset)
    print("=" * 80)
    print("RESULT")
    print(f"samples:                    {len(samples)}")
    print(f"dropped/invalid:            {dropped}/{invalid}")
    print(f"max neutral deviation:      {max_deviation:.3f} deg")
    print(f"rms neutral deviation:      {rms_deviation:.3f} deg")
    print("neutral angular speed p95:  " + (f"{p95:.4f} rad/s" if p95 is not None else "<not available>"))
    print("mean input xyzw:            [" + ", ".join(f"{v:+.9f}" for v in mean) + "]")
    print("post orientation offset:    [" + ", ".join(f"{v:+.9f}" for v in offset) + "]")
    print(f"predicted identity error:   {q_distance_deg(predicted, (0.0, 0.0, 0.0, 1.0)):.9f} deg")

    if problems:
        print("\nCALIBRATION NOT RELIABLE - config was not changed.")
        for problem in problems:
            print(f"  - {problem}")
        return 2

    print("\nCALIBRATION PASSED")
    print()
    print(json.dumps({"orientation_offset": block}, indent=2))
    if args.write:
        if config_path is None or config is None or config_device is None:
            raise RuntimeError("internal error: write requested without config")
        backup = write_config(config_path, config, config_device, block)
        print("\nCONFIG UPDATED")
        print(f"config: {config_path}")
        print(f"backup: {backup}")
        print("Restart override_controller to apply the new offset.")
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
