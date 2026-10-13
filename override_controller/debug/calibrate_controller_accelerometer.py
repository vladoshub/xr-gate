#!/usr/bin/env python3
"""Six-position accelerometer calibration stored in the selected controller config.

Uses Python's standard library and the controller_input SHM stream on Linux.
Run with --config <active-profile.json> --device-id <devices[].id>.
Disable this device's imu_calibrations entry and restart override_controller
before capture. Cameras and optical tracking are not needed.
"""

from __future__ import annotations

import argparse
import json
import math
import mmap
import os
from pathlib import Path
import statistics
import sys
import tempfile
import time

from record_controller_imu import COLUMNS, FRAME_SIZE, RING, SLOT, decode_payload, read_slot

POSES = ("+X", "-X", "+Y", "-Y", "+Z", "-Z")
ACCEL_VALID = 2
GYRO_VALID = 1
ACCEL_CALIBRATED = 64
IMU_ACTIVE = 3
CAP_REQUIRED = 3  # gyroscope + accelerometer


def norm(v):
    return math.sqrt(sum(x * x for x in v))


def config_device(config, device_id):
    devices = [d for d in config.get("devices", []) if d.get("id") == device_id]
    if device_id <= 0 or len(devices) != 1:
        raise ValueError("device-id must reference exactly one devices[].id")
    if not isinstance(config.get("imu_calibrations", []), list):
        raise ValueError("imu_calibrations must be an array")
    ids = [entry.get("device_id") for entry in config.get("imu_calibrations", [])]
    if any(type(i) is not int or i <= 0 for i in ids) or len(set(ids)) != len(ids):
        raise ValueError("imu_calibrations requires unique positive integer device_id values")
    return devices[0]


def inverse_basis(v, device):
    """Undo Rz*Ry*Rx*axis_flips used by apply_orientation_transform.

    Calibration is fitted in native sensor axes, never the mounting basis.
    orientation_offset affects only the quaternion and does not enter here.
    """
    transform = device.get("orientation_transform", {})
    if not transform.get("enabled", False):
        return tuple(v)
    basis = transform.get("basis_rotation", {})
    x, y, z = v
    rz, ry, rx = [math.radians(float(basis.get(key, 0)))
                  for key in ("rz_deg", "ry_deg", "rx_deg")]
    c, s = math.cos(rz), math.sin(rz)
    x, y = c * x + s * y, -s * x + c * y
    c, s = math.cos(ry), math.sin(ry)
    x, z = c * x - s * z, s * x + c * z
    c, s = math.cos(rx), math.sin(rx)
    y, z = c * y + s * z, -s * y + c * z
    return tuple(-value if transform.get(f"invert_{axis}", False) else value
                 for axis, value in zip("xyz", (x, y, z)))


def native_sample(row, device):
    if int(row["sample_index"]) < 0 or int(row["imu_status"]) != IMU_ACTIVE:
        return None
    if int(row["capability_flags"]) & CAP_REQUIRED != CAP_REQUIRED:
        raise ValueError("Selected controller must have a gyroscope and accelerometer")
    flags = int(row["sample_flags"]) | int(row["data_flags"])
    if flags & ACCEL_CALIBRATED:
        raise ValueError("Accelerometer correction is active. Set enabled=false for this "
                         "device_id in imu_calibrations and restart override_controller first")
    if int(row["sample_flags"]) & (ACCEL_VALID | GYRO_VALID) != ACCEL_VALID | GYRO_VALID:
        return None
    wanted_hash = int(str(device.get("stable_hash", "0")), 16)
    if wanted_hash and row["device_id"] != f"{wanted_hash:016x}":
        raise ValueError("SHM side is routed to another device; use the active connected config")
    force = inverse_basis([float(row[key]) for key in ("ax", "ay", "az")], device)
    gyro = [float(row[key]) for key in ("gx", "gy", "gz")]
    if not all(math.isfinite(x) for x in (*force, *gyro)):
        return None
    return int(row["sample_ns"]), force, norm(gyro)


def pose_matches(force, pose, gravity):
    axis = "XYZ".index(pose[1])
    sign = 1 if pose[0] == "+" else -1
    magnitude = norm(force)
    return (0.75 * gravity < magnitude < 1.25 * gravity and
            sign * force[axis] > 0.96 * magnitude)


def solve_linear(matrix, rhs):
    a = [list(row) + [value] for row, value in zip(matrix, rhs)]
    n = len(rhs)
    for col in range(n):
        pivot = max(range(col, n), key=lambda row: abs(a[row][col]))
        if abs(a[pivot][col]) < 1e-9:
            raise ValueError("Six distinct orientations are required; calibration fit is singular")
        a[col], a[pivot] = a[pivot], a[col]
        divisor = a[col][col]
        a[col] = [value / divisor for value in a[col]]
        for row in range(n):
            if row == col:
                continue
            factor = a[row][col]
            a[row] = [value - factor * other for value, other in zip(a[row], a[col])]
    return [row[-1] for row in a]


def fit_calibration(captures, gravity=9.80665):
    if not math.isfinite(gravity) or not 7 < gravity < 12:
        raise ValueError("gravity must be between 7 and 12 m/s²")
    if set(captures) != set(POSES):
        raise ValueError("All six poses +X/-X/+Y/-Y/+Z/-Z are required")
    all_samples, means = [], []
    for pose in POSES:
        samples = captures[pose]
        if len(samples) < 100:
            raise ValueError(f"{pose}: at least 100 stationary samples are required")
        if any(len(v) != 3 or not all(math.isfinite(x) for x in v) for v in samples):
            raise ValueError("Samples must contain three finite native-axis SI values")
        mean = [statistics.fmean(v[axis] for v in samples) for axis in range(3)]
        std = norm([statistics.pstdev(v[axis] for v in samples) for axis in range(3)])
        if std > 0.06 or not pose_matches(mean, pose, gravity):
            raise ValueError(f"{pose}: unstable or wrong orientation (force std={std:.4f})")
        means.append(mean)
        all_samples.extend(samples)
    # Fit a diagonal ellipsoid: sum(A_i*x_i²+B_i*x_i)=1. Normalize SI
    # forces by g for conditioning, then recover center and axis radii.
    features = [[(x / gravity) ** 2 for x in v] + [x / gravity for x in v]
                for v in all_samples]
    matrix = [[math.fsum(v[i] * v[j] for v in features) for j in range(6)]
              for i in range(6)]
    rhs = [math.fsum(v[i] for v in features) for i in range(6)]
    coefficients = solve_linear(matrix, rhs)
    a, b = coefficients[:3], coefficients[3:]
    if any(value <= 0 for value in a):
        raise ValueError("Fit is not a physically valid accelerometer ellipsoid")
    center = [-linear / (2 * quadratic) for linear, quadratic in zip(b, a)]
    constant = 1 + sum(quadratic * offset ** 2 for quadratic, offset in zip(a, center))
    bias = [x * gravity for x in center]
    scale = [math.sqrt(x / constant) for x in a]
    if any(abs(x) > 2 for x in bias) or any(not 0.5 <= x <= 2 for x in scale):
        raise ValueError("Bias or scale is outside the runtime calibration limits")
    errors = [norm([(x - offset) * factor for x, offset, factor in zip(v, bias, scale)])
              - gravity for v in all_samples]
    rms = math.sqrt(statistics.fmean(x * x for x in errors))
    if rms > 0.08 or max(abs(x) for x in errors) > 0.25:
        raise ValueError(f"Stationary gravity residual is too large: rms={rms:.4f} m/s²")
    return {"enabled": True, "accelerometer_bias_m_s2": bias,
            "accelerometer_scale": scale}, {"samples": len(all_samples),
                                             "gravity_rms_m_s2": rms}


def write_config(path, original_text, device_id, calibration):
    if path.read_text(encoding="utf-8") != original_text:
        raise ValueError("Config changed during capture; no values written. Save the report and refit")
    config = json.loads(original_text)
    config_device(config, device_id)
    entry = {"device_id": device_id, **calibration}
    entries = config.setdefault("imu_calibrations", [])
    for index, old in enumerate(entries):
        if old["device_id"] == device_id:
            entries[index] = {**old, **entry}
            break
    else:
        entries.append(entry)
    backup = path.with_name(path.name + f".calibration-{time.time_ns()}.bak")
    backup.write_text(original_text, encoding="utf-8")
    # Preserve permissions and replace the whole selected profile atomically.
    descriptor, temporary = tempfile.mkstemp(prefix=path.name + ".", suffix=".tmp", dir=path.parent)
    try:
        os.fchmod(descriptor, path.stat().st_mode & 0o777)
        with os.fdopen(descriptor, "w", encoding="utf-8") as handle:
            handle.write(json.dumps(config, indent=2, ensure_ascii=False, allow_nan=False) + "\n")
            handle.flush()
            os.fsync(handle.fileno())
        os.replace(temporary, path)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)
    return backup


class StreamReader:
    def __init__(self, registry, stream_id):
        with open(registry, encoding="utf-8") as handle:
            streams = json.load(handle).get("streams", {})
        if stream_id not in streams:
            raise ValueError(f"Start override_controller first: stream {stream_id!r} is absent")
        entry = streams[stream_id]
        if entry.get("transport", "shm") != "shm":
            raise ValueError("This capture tool requires a Linux SHM controller_input stream")
        defaults = dict(header_size=4096, slot_count=1024, slot_stride=128 + FRAME_SIZE,
                        slot_header_size=128, payload_size=FRAME_SIZE)
        self.layout = {key: int(entry.get(key, value)) for key, value in defaults.items()}
        v = self.layout
        if (min(v.values()) <= 0 or v["header_size"] < RING.size or
                v["slot_header_size"] < SLOT.size or v["payload_size"] < FRAME_SIZE or
                v["slot_stride"] < v["slot_header_size"] + v["payload_size"]):
            raise ValueError("Invalid controller SHM ring layout")
        path = Path("/dev/shm") / str(entry.get("shm_name", stream_id)).lstrip("/")
        self.handle = path.open("rb")
        try:
            self.mapping = mmap.mmap(self.handle.fileno(), 0, access=mmap.ACCESS_READ)
        except BaseException:
            self.handle.close()
            raise
        if len(self.mapping) < v["header_size"] + v["slot_count"] * v["slot_stride"]:
            self.close()
            raise ValueError("Controller SHM ring is shorter than its registry layout")
        self.last = RING.unpack_from(self.mapping)[-1]

    def close(self):
        self.mapping.close()
        self.handle.close()

    def rows(self):
        latest = RING.unpack_from(self.mapping)[-1]
        if latest < self.last:
            raise ValueError("Publisher restarted during capture; restart the calibration tool")
        first = max(self.last + 1, latest - self.layout["slot_count"] + 1)
        if first > self.last + 1:
            raise ValueError("Controller ring overflow during capture; repeat the calibration")
        for sequence in range(first, latest + 1):
            payload = read_slot(self.mapping, self.layout, sequence)
            if payload is None:
                break
            self.last = sequence
            for values in decode_payload(payload, time.monotonic_ns(), 0):
                yield dict(zip(COLUMNS, values))


def capture_pose(reader, device, pose, gravity, hold_sec, timeout_sec):
    print(f"\n{pose}: поверните контроллер так, чтобы {pose} смотрела вверх; удерживайте неподвижно.", flush=True)
    print("Подсказка: смотрите native XYZ ниже; нужная ось должна быть около ±9.8.", flush=True)
    samples, start_ns, last_ns, epoch = [], None, 0, None
    deadline, next_print = time.monotonic() + timeout_sec, 0
    while time.monotonic() < deadline:
        for row in reader.rows():
            if row["side"] != device["imu_side"]:
                continue
            sample = native_sample(row, device)
            if sample is None:
                samples, start_ns = [], None
                continue
            ns, force, gyro = sample
            row_epoch = row["epoch"]
            if ns <= last_ns and row_epoch == epoch:
                continue
            if row_epoch != epoch or ns <= last_ns or ns - last_ns > 100_000_000:
                samples, start_ns = [], None
            last_ns, epoch = ns, row_epoch
            if gyro > 0.12 or not pose_matches(force, pose, gravity):
                samples, start_ns = [], None
            else:
                if start_ns is None:
                    start_ns = ns
                samples.append(force)
                # Reject shifts during a hold, including slow motion with quiet gyro.
                if len(samples) >= 20:
                    std = norm([statistics.pstdev(v[i] for v in samples) for i in range(3)])
                    if std > 0.06:
                        samples, start_ns = [force], ns
                elapsed = (ns - start_ns) / 1e9
                if elapsed >= hold_sec and len(samples) >= 100:
                    print(f"{pose}: OK, {len(samples)} samples, {elapsed:.1f} sec", flush=True)
                    return samples
            if time.monotonic() >= next_print:
                elapsed = 0 if start_ns is None else (ns - start_ns) / 1e9
                print("native XYZ=(%+.2f, %+.2f, %+.2f) gyro=%.3f hold=%.1f/%.1fs" %
                      (*force, gyro, elapsed, hold_sec), flush=True)
                next_print = time.monotonic() + 1
        time.sleep(0.002)
    raise ValueError(f"{pose}: no stable raw IMU data within {timeout_sec:g} sec; no config written")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", type=Path, required=True, help="active controller profile")
    parser.add_argument("--device-id", type=int, required=True, help="devices[].id, not a BLE UUID")
    parser.add_argument("--registry", default=os.environ.get("CONTROLLER_INPUT_REGISTRY", "/tmp/tracking_streams.json"))
    parser.add_argument("--stream", default=os.environ.get("CONTROLLER_INPUT_STREAM", "controller_input"))
    parser.add_argument("--gravity", type=float, help="match backend gravity_magnitude; default 9.80665")
    parser.add_argument("--hold-sec", type=float, default=3.0)
    parser.add_argument("--timeout-sec", type=float, default=120.0, help="timeout per pose")
    parser.add_argument("--samples", type=Path, help="refit a previously saved native-axis capture")
    parser.add_argument("--output", type=Path, help="save captures and fit for later inspection")
    parser.add_argument("--write-config", action="store_true", help="write selected entry, with backup, after validation")
    args = parser.parse_args()
    if not (math.isfinite(args.hold_sec) and math.isfinite(args.timeout_sec) and
            2 <= args.hold_sec <= 10 and args.timeout_sec > args.hold_sec):
        parser.error("hold-sec must be 2..10 and timeout-sec must exceed it")
    try:
        args.config = args.config.expanduser().resolve()
        original = args.config.read_text(encoding="utf-8")
        config = json.loads(original)
        device = config_device(config, args.device_id)
        if args.output and args.output.expanduser().resolve() == args.config:
            raise ValueError("output must not overwrite the controller config")
        if args.samples:
            report = json.loads(args.samples.read_text(encoding="utf-8"))
            if report.get("format") != "XR_ACCELEROMETER_CAPTURE_V1" or report.get("device_id") != args.device_id:
                raise ValueError("Capture must be XR_ACCELEROMETER_CAPTURE_V1 for this device_id")
            captures = report["native_samples_m_s2"]
            gravity = args.gravity if args.gravity is not None else report["gravity_m_s2"]
        else:
            if device.get("imu_side") not in ("left", "right"):
                raise ValueError("Selected device requires explicit imu_side=left/right in the active config")
            entry = next((c for c in config.get("imu_calibrations", [])
                          if c["device_id"] == args.device_id), {})
            if entry and entry.get("enabled", True):
                raise ValueError("Set this device_id calibration enabled=false and restart override_controller first")
            gravity = args.gravity if args.gravity is not None else 9.80665
            if not math.isfinite(gravity) or not 7 < gravity < 12:
                raise ValueError("gravity must be between 7 and 12 m/s²")
            reader = StreamReader(args.registry, args.stream)
            try:
                captures = {pose: capture_pose(reader, device, pose, gravity,
                                              args.hold_sec, args.timeout_sec) for pose in POSES}
            finally:
                reader.close()
        calibration, quality = fit_calibration(captures, gravity)
        report = {"format": "XR_ACCELEROMETER_CAPTURE_V1", "device_id": args.device_id,
                  "gravity_m_s2": gravity, "native_samples_m_s2": captures,
                  "calibration": {"device_id": args.device_id, **calibration}, "quality": quality}
        if args.output:
            args.output.parent.mkdir(parents=True, exist_ok=True)
            args.output.write_text(json.dumps(report, indent=2, allow_nan=False) + "\n", encoding="utf-8")
            print(f"Capture saved: {args.output}")
        print(json.dumps(report["calibration"], indent=2))
        print("Gravity RMS: %.5f m/s²" % quality["gravity_rms_m_s2"])
        if args.write_config:
            backup = write_config(args.config, original, args.device_id, calibration)
            print(f"Updated {args.config}; backup: {backup}\nRestart override_controller to apply.")
        else:
            print("Config unchanged; use --write-config to save the validated result.")
    except (ValueError, OSError, KeyError, TypeError) as error:
        print(f"[accelerometer_calibration] ERROR: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
