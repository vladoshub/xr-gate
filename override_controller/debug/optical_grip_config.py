"""Shared, dependency-free config checks for optical controller grip tools."""
from copy import deepcopy
import math

RECORDING_SCHEMA = "xr_controller_guided_grip_recording_v2"
REPORT_SCHEMA = "xr_controller_guided_grip_report_v2"
CANDIDATE_SCHEMA = "xr_controller_optical_grip_trial_v2"
LEGACY_RECORDING_SCHEMA = "gearvr_guided_grip_recording_v1"
LEGACY_CANDIDATE_SCHEMA = "gearvr_optical_grip_trial_v1"
MIN_GRAVITY_SAMPLES = 10
MAX_UP_ERROR_DEG = 8
MAX_UP_SPREAD_DEG = 5


def validate_transform(value):
    if not isinstance(value, dict):
        raise ValueError("orientation_transform must be an object")
    for key in ("enabled", "invert_x", "invert_y", "invert_z"):
        if not isinstance(value.get(key, False), bool):
            raise ValueError("orientation_transform." + key + " must be boolean")
    basis = value.get("basis_rotation", {})
    if not isinstance(basis, dict):
        raise ValueError("orientation_transform.basis_rotation must be an object")
    for key in ("rx_deg", "ry_deg", "rz_deg"):
        angle = basis.get(key, 0)
        if isinstance(angle, bool) or not isinstance(angle, (int, float)) or not math.isfinite(angle):
            raise ValueError("orientation_transform.basis_rotation must contain finite angles")
    return deepcopy(value)


def normalized_quaternion(value):
    if not isinstance(value, (list, tuple)) or len(value) != 4 or any(
            isinstance(x, bool) or not isinstance(x, (int, float)) or not math.isfinite(x) for x in value):
        raise ValueError("Expected finite quaternion_xyzw with four numbers")
    norm = math.sqrt(sum(x * x for x in value))
    if not math.isfinite(norm) or norm < 1e-6:
        raise ValueError("Expected nonzero quaternion_xyzw")
    return [x / norm for x in value]


def validate_offset(value):
    if not isinstance(value, dict) or not isinstance(value.get("enabled", False), bool):
        raise ValueError("orientation_offset must be an object with boolean enabled")
    if value.get("enabled"):
        normalized_quaternion(value.get("quaternion_xyzw"))
        if value.get("multiply_order", "post") not in ("pre", "world", "post", "local"):
            raise ValueError("Unknown orientation_offset.multiply_order")
    return deepcopy(value)


def legacy_basis(value):
    """Only old recordings/candidates lack measured gravity; preserve their guard."""
    value = validate_transform(value)
    basis = value.get("basis_rotation", {})
    return (value.get("enabled", False) and
            not any(value.get(k, False) for k in ("invert_x", "invert_y", "invert_z")) and
            all(abs(basis.get(k, 0) - v) <= 1e-6 for k, v in
                (("rx_deg", -90), ("ry_deg", 0), ("rz_deg", 0))))


def config_device_snapshot(device):
    fingerprint = device.get("fingerprint", {})
    if not isinstance(fingerprint, dict):
        raise ValueError("Device fingerprint must be an object")
    return {"orientation_transform": validate_transform(device.get("orientation_transform", {})),
            "orientation_offset": validate_offset(device.get("orientation_offset", {})),
            "fingerprint": deepcopy(fingerprint)}
