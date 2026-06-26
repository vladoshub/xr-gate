#!/usr/bin/env python3
"""
XR runtime debug viewer.

Reads XR SHM ring streams through registry JSON files and displays a simple
2D live view for the coordinates that are about to go into runtime drivers.

Views:
  - X/Z top view
  - X/Y front/vertical view

Default streams:
  - runtime_hmd_pose      from /tmp/runtime_tracking_streams.json  (green)
  - runtime_hand_tracking from /tmp/runtime_tracking_streams.json  (blue, 21-joint runtime hand)
  - hand_skeleton26       from /tmp/tracking_streams.json          (red, future Ultraleap/OpenXR)
  - runtime_body_trackers from /tmp/runtime_tracking_streams.json  (brown)

The viewer intentionally keeps source-specific transforms in config.  This lets
us debug axis inversions, axis swaps, and HMD-relative streams before touching
Monado/OpenVR driver code.
"""

from __future__ import annotations

import argparse
import ctypes
import json
import math
import mmap
import os
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Dict, Iterable, List, Optional, Tuple

try:
    import matplotlib.pyplot as plt
except Exception as exc:  # pragma: no cover - runtime dependency check
    print("[ERROR] matplotlib is required. Install: sudo apt install python3-matplotlib", file=sys.stderr)
    raise

try:
    import yaml  # type: ignore
except Exception:  # pragma: no cover - optional until config load
    yaml = None


# ---------------------------------------------------------------------------
# ABI constants. Keep these in sync with shared/include/* contracts.
# ---------------------------------------------------------------------------

RUNTIME_HMD_POSE_FORMAT_NAME = "RUNTIME_HMD_POSE_V1"
HMD_POSE_FORMAT_NAME = "HMD_POSE_F64_LE"
HAND_TRACKING_21_JOINT_FORMAT_NAMES = {"HAND_TRACKING_21_JOINT_F32_V2", "HAND_TRACKING_F32_V2", "HAND_TRACKING_V2"}
HAND_SKELETON26_FORMAT_NAME = "HAND_SKELETON26_F32_V1"
BODY_TRACKER_SET_FORMAT_NAME = "BODY_TRACKER_SET_F32_V1"

RUNTIME_HMD_FLAG_POSE_VALID = 1 << 0
HMD_FLAG_POSE_VALID = 1 << 0
HAND_POSE_VALID = 1 << 0
HAND_FLAG_LEFT_VALID = 1 << 0
HAND_FLAG_RIGHT_VALID = 1 << 1
HAND_FLAG_JOINTS_VALID = 1 << 2
HAND_SKELETON26_FRAME_LEFT_VALID = 1 << 0
HAND_SKELETON26_FRAME_RIGHT_VALID = 1 << 1
HAND_SKELETON26_SIDE_POSE_VALID = 1 << 0
HAND_SKELETON26_SIDE_JOINTS_VALID = 1 << 1
HAND_SKELETON26_JOINT_POSITION_VALID = 1 << 0
HAND_SKELETON26_JOINT_TRACKED = 1 << 3
BODY_TRACKER_FLAG_POSE_VALID = 1 << 0
BODY_TRACKER_FLAG_POSITION_VALID = 1 << 1
BODY_TRACKER_FLAG_CONNECTED = 1 << 6

HAND_JOINT_COUNT_V2 = 21
HAND_SKELETON26_JOINT_COUNT = 26
BODY_TRACKER_MAX_TRACKERS = 32

BODY_TRACKER_ROLES = {
    0: "unknown",
    1: "generic",
    2: "waist",
    3: "chest",
    4: "left_foot",
    5: "right_foot",
    6: "left_knee",
    7: "right_knee",
    8: "left_elbow",
    9: "right_elbow",
    10: "left_shoulder",
    11: "right_shoulder",
    12: "head",
    13: "camera",
}


# ---------------------------------------------------------------------------
# C ABI structures.
# ---------------------------------------------------------------------------


class RingHeaderV1(ctypes.LittleEndianStructure):
    _pack_ = 1
    _fields_ = [
        ("magic", ctypes.c_char * 8),
        ("version", ctypes.c_uint32),
        ("header_size", ctypes.c_uint32),
        ("slot_count", ctypes.c_uint32),
        ("slot_stride", ctypes.c_uint32),
        ("slot_header_size", ctypes.c_uint32),
        ("payload_size", ctypes.c_uint32),
        ("reserved0", ctypes.c_uint32),
        ("latest_sequence", ctypes.c_uint64),
    ]


class RingSlotHeaderV1(ctypes.LittleEndianStructure):
    _pack_ = 1
    _fields_ = [
        ("seq_begin", ctypes.c_uint64),
        ("seq_end", ctypes.c_uint64),
        ("timestamp_ns", ctypes.c_uint64),
        ("source_timestamp_ns", ctypes.c_uint64),
        ("payload_size", ctypes.c_uint32),
        ("flags", ctypes.c_uint32),
        ("reserved", ctypes.c_uint8 * 88),
    ]


class RuntimeHmdPoseF64V1(ctypes.LittleEndianStructure):
    _pack_ = 1
    _fields_ = [
        ("version", ctypes.c_uint32),
        ("size_bytes", ctypes.c_uint32),
        ("sequence", ctypes.c_uint64),
        ("timestamp_ns", ctypes.c_uint64),
        ("source_timestamp_ns", ctypes.c_uint64),
        ("target_timestamp_ns", ctypes.c_uint64),
        ("source_sequence", ctypes.c_uint64),
        ("reset_counter", ctypes.c_uint64),
        ("px", ctypes.c_double),
        ("py", ctypes.c_double),
        ("pz", ctypes.c_double),
        ("qw", ctypes.c_double),
        ("qx", ctypes.c_double),
        ("qy", ctypes.c_double),
        ("qz", ctypes.c_double),
        ("vx", ctypes.c_double),
        ("vy", ctypes.c_double),
        ("vz", ctypes.c_double),
        ("wx", ctypes.c_double),
        ("wy", ctypes.c_double),
        ("wz", ctypes.c_double),
        ("tracking_status", ctypes.c_uint32),
        ("flags", ctypes.c_uint32),
        ("confidence", ctypes.c_float),
        ("source_tracking_status", ctypes.c_uint32),
        ("source_flags", ctypes.c_uint32),
        ("reserved0", ctypes.c_uint32),
    ]


class HmdPoseF64V1(ctypes.LittleEndianStructure):
    _pack_ = 1
    _fields_ = [
        ("version", ctypes.c_uint32),
        ("size_bytes", ctypes.c_uint32),
        ("sequence", ctypes.c_uint64),
        ("timestamp_ns", ctypes.c_uint64),
        ("source_timestamp_ns", ctypes.c_uint64),
        ("reset_counter", ctypes.c_uint64),
        ("px", ctypes.c_double),
        ("py", ctypes.c_double),
        ("pz", ctypes.c_double),
        ("qw", ctypes.c_double),
        ("qx", ctypes.c_double),
        ("qy", ctypes.c_double),
        ("qz", ctypes.c_double),
        ("vx", ctypes.c_double),
        ("vy", ctypes.c_double),
        ("vz", ctypes.c_double),
        ("wx", ctypes.c_double),
        ("wy", ctypes.c_double),
        ("wz", ctypes.c_double),
        ("tracking_status", ctypes.c_uint32),
        ("flags", ctypes.c_uint32),
        ("confidence", ctypes.c_float),
        ("reserved0", ctypes.c_uint32),
    ]


class HandJointF32V2(ctypes.LittleEndianStructure):
    _pack_ = 1
    _fields_ = [
        ("joint_id", ctypes.c_uint32),
        ("flags", ctypes.c_uint32),
        ("px", ctypes.c_float),
        ("py", ctypes.c_float),
        ("pz", ctypes.c_float),
        ("qw", ctypes.c_float),
        ("qx", ctypes.c_float),
        ("qy", ctypes.c_float),
        ("qz", ctypes.c_float),
        ("radius_m", ctypes.c_float),
        ("confidence", ctypes.c_float),
    ]


class HandSideF32V2(ctypes.LittleEndianStructure):
    _pack_ = 1
    _fields_ = [
        ("handedness", ctypes.c_uint32),
        ("status", ctypes.c_uint32),
        ("flags", ctypes.c_uint32),
        ("confidence", ctypes.c_float),
        ("controller_px", ctypes.c_float),
        ("controller_py", ctypes.c_float),
        ("controller_pz", ctypes.c_float),
        ("controller_qw", ctypes.c_float),
        ("controller_qx", ctypes.c_float),
        ("controller_qy", ctypes.c_float),
        ("controller_qz", ctypes.c_float),
        ("palm_px", ctypes.c_float),
        ("palm_py", ctypes.c_float),
        ("palm_pz", ctypes.c_float),
        ("palm_qw", ctypes.c_float),
        ("palm_qx", ctypes.c_float),
        ("palm_qy", ctypes.c_float),
        ("palm_qz", ctypes.c_float),
        ("wrist_px", ctypes.c_float),
        ("wrist_py", ctypes.c_float),
        ("wrist_pz", ctypes.c_float),
        ("wrist_qw", ctypes.c_float),
        ("wrist_qx", ctypes.c_float),
        ("wrist_qy", ctypes.c_float),
        ("wrist_qz", ctypes.c_float),
        ("vx", ctypes.c_float),
        ("vy", ctypes.c_float),
        ("vz", ctypes.c_float),
        ("wx", ctypes.c_float),
        ("wy", ctypes.c_float),
        ("wz", ctypes.c_float),
        ("pinch_strength", ctypes.c_float),
        ("grab_strength", ctypes.c_float),
        ("pinch_active", ctypes.c_uint32),
        ("grab_active", ctypes.c_uint32),
        ("joint_count", ctypes.c_uint32),
        ("reserved0", ctypes.c_uint32),
        ("joints", HandJointF32V2 * HAND_JOINT_COUNT_V2),
    ]


class HandTrackingFrameF32V2(ctypes.LittleEndianStructure):
    _pack_ = 1
    _fields_ = [
        ("version", ctypes.c_uint32),
        ("size_bytes", ctypes.c_uint32),
        ("sequence", ctypes.c_uint64),
        ("timestamp_ns", ctypes.c_uint64),
        ("source_timestamp_ns", ctypes.c_uint64),
        ("reset_counter", ctypes.c_uint64),
        ("tracking_status", ctypes.c_uint32),
        ("flags", ctypes.c_uint32),
        ("confidence", ctypes.c_float),
        ("hand_count", ctypes.c_uint32),
        ("left", HandSideF32V2),
        ("right", HandSideF32V2),
    ]


class HandSkeleton26JointF32V1(ctypes.LittleEndianStructure):
    _pack_ = 1
    _fields_ = [
        ("joint_id", ctypes.c_uint32),
        ("flags", ctypes.c_uint32),
        ("px", ctypes.c_float),
        ("py", ctypes.c_float),
        ("pz", ctypes.c_float),
        ("qw", ctypes.c_float),
        ("qx", ctypes.c_float),
        ("qy", ctypes.c_float),
        ("qz", ctypes.c_float),
        ("radius_m", ctypes.c_float),
        ("confidence", ctypes.c_float),
    ]


class HandSkeleton26SideF32V1(ctypes.LittleEndianStructure):
    _pack_ = 1
    _fields_ = [
        ("handedness", ctypes.c_uint32),
        ("status", ctypes.c_uint32),
        ("flags", ctypes.c_uint32),
        ("confidence", ctypes.c_float),
        ("joint_count", ctypes.c_uint32),
        ("reserved0", ctypes.c_uint32),
        ("pinch_strength", ctypes.c_float),
        ("grab_strength", ctypes.c_float),
        ("pinch_active", ctypes.c_uint32),
        ("grab_active", ctypes.c_uint32),
        ("joints", HandSkeleton26JointF32V1 * HAND_SKELETON26_JOINT_COUNT),
    ]


class HandSkeleton26FrameF32V1(ctypes.LittleEndianStructure):
    _pack_ = 1
    _fields_ = [
        ("version", ctypes.c_uint32),
        ("size_bytes", ctypes.c_uint32),
        ("sequence", ctypes.c_uint64),
        ("timestamp_ns", ctypes.c_uint64),
        ("source_timestamp_ns", ctypes.c_uint64),
        ("reset_counter", ctypes.c_uint64),
        ("space", ctypes.c_uint32),
        ("source", ctypes.c_uint32),
        ("flags", ctypes.c_uint32),
        ("hand_count", ctypes.c_uint32),
        ("left", HandSkeleton26SideF32V1),
        ("right", HandSkeleton26SideF32V1),
    ]


class BodyTrackerPoseF32V1(ctypes.LittleEndianStructure):
    _pack_ = 1
    _fields_ = [
        ("px", ctypes.c_float),
        ("py", ctypes.c_float),
        ("pz", ctypes.c_float),
        ("qw", ctypes.c_float),
        ("qx", ctypes.c_float),
        ("qy", ctypes.c_float),
        ("qz", ctypes.c_float),
        ("vx", ctypes.c_float),
        ("vy", ctypes.c_float),
        ("vz", ctypes.c_float),
        ("wx", ctypes.c_float),
        ("wy", ctypes.c_float),
        ("wz", ctypes.c_float),
    ]


class BodyTrackerF32V1(ctypes.LittleEndianStructure):
    _pack_ = 1
    _fields_ = [
        ("tracker_index", ctypes.c_uint32),
        ("role", ctypes.c_uint32),
        ("status", ctypes.c_uint32),
        ("flags", ctypes.c_uint32),
        ("stable_id_hash", ctypes.c_uint64),
        ("device_serial_hash", ctypes.c_uint64),
        ("confidence", ctypes.c_float),
        ("battery", ctypes.c_float),
        ("reserved0", ctypes.c_uint32),
        ("reserved1", ctypes.c_uint32),
        ("pose", BodyTrackerPoseF32V1),
        ("tracker_id", ctypes.c_char * 64),
    ]


class BodyTrackerSetFrameF32V1(ctypes.LittleEndianStructure):
    _pack_ = 1
    _fields_ = [
        ("version", ctypes.c_uint32),
        ("size_bytes", ctypes.c_uint32),
        ("sequence", ctypes.c_uint64),
        ("timestamp_ns", ctypes.c_uint64),
        ("source_timestamp_ns", ctypes.c_uint64),
        ("reset_counter", ctypes.c_uint64),
        ("space", ctypes.c_uint32),
        ("source", ctypes.c_uint32),
        ("flags", ctypes.c_uint32),
        ("tracker_count", ctypes.c_uint32),
        ("trackers", BodyTrackerF32V1 * BODY_TRACKER_MAX_TRACKERS),
    ]


# Defensive ABI checks. They catch accidental Python/C++ layout drift early.
assert ctypes.sizeof(RingSlotHeaderV1) == 128
assert ctypes.sizeof(RuntimeHmdPoseF64V1) == 184
assert ctypes.sizeof(HmdPoseF64V1) == 160
assert ctypes.sizeof(HandJointF32V2) == 44
assert ctypes.sizeof(HandSideF32V2) == 1072
assert ctypes.sizeof(HandTrackingFrameF32V2) == 2200
assert ctypes.sizeof(HandSkeleton26JointF32V1) == 44
assert ctypes.sizeof(HandSkeleton26FrameF32V1) == 2424
assert ctypes.sizeof(BodyTrackerSetFrameF32V1) == 5304


# ---------------------------------------------------------------------------
# Config and transforms.
# ---------------------------------------------------------------------------


@dataclass
class TransformConfig:
    axis_map: Tuple[str, str, str] = ("x", "y", "z")
    invert_x: bool = False
    invert_y: bool = False
    invert_z: bool = False
    rotation_deg: Tuple[float, float, float] = (0.0, 0.0, 0.0)
    scale: float = 1.0
    offset_m: Tuple[float, float, float] = (0.0, 0.0, 0.0)


@dataclass
class HmdRelativeConfig:
    enabled: bool = False
    # add: output = point + hmd + offset
    # subtract: output = point - hmd + offset
    # none: output = point + offset
    mode: str = "add"
    offset_m: Tuple[float, float, float] = (0.0, 0.0, 0.0)


@dataclass
class StreamConfig:
    enabled: bool = True
    registry: str = "/tmp/runtime_tracking_streams.json"
    stream: str = ""
    stale_ms: float = 1000.0
    coordinate_transform: TransformConfig = field(default_factory=TransformConfig)
    hmd_relative: HmdRelativeConfig = field(default_factory=HmdRelativeConfig)


@dataclass
class ViewerConfig:
    refresh_hz: float = 30.0
    registry_poll_interval_s: float = 1.0
    autoscale: bool = False
    x_lim: Tuple[float, float] = (-1.5, 1.5)
    y_lim: Tuple[float, float] = (-1.0, 1.5)
    z_lim: Tuple[float, float] = (-1.5, 1.5)
    hmd: StreamConfig = field(default_factory=lambda: StreamConfig(
        enabled=True,
        registry="/tmp/runtime_tracking_streams.json",
        stream="runtime_hmd_pose",
        stale_ms=1000.0,
    ))
    hand_tracking_21_joint: StreamConfig = field(default_factory=lambda: StreamConfig(
        enabled=True,
        registry="/tmp/runtime_tracking_streams.json",
        stream="runtime_hand_tracking",
        stale_ms=1000.0,
    ))
    hand_skeleton26: StreamConfig = field(default_factory=lambda: StreamConfig(
        # 21-joint/runtime_hand_tracking is the default hand source.
        # Enable this explicitly when testing future Ultraleap/OpenXR 26-joint input.
        enabled=False,
        registry="/tmp/tracking_streams.json",
        stream="hand_skeleton26",
        stale_ms=1000.0,
    ))
    body_trackers: StreamConfig = field(default_factory=lambda: StreamConfig(
        enabled=True,
        registry="/tmp/runtime_tracking_streams.json",
        stream="runtime_body_trackers",
        stale_ms=1000.0,
    ))


def _tuple3(value: Any, default: Tuple[float, float, float]) -> Tuple[float, float, float]:
    if value is None:
        return default
    if isinstance(value, dict):
        return (float(value.get("x", default[0])),
                float(value.get("y", default[1])),
                float(value.get("z", default[2])))
    if isinstance(value, (list, tuple)) and len(value) == 3:
        return (float(value[0]), float(value[1]), float(value[2]))
    raise ValueError(f"expected 3-vector, got {value!r}")


def _axis_map(value: Any) -> Tuple[str, str, str]:
    if value is None:
        return ("x", "y", "z")
    if isinstance(value, str):
        parts = [p.strip() for p in value.split(",") if p.strip()]
    else:
        parts = list(value)
    if len(parts) != 3:
        raise ValueError(f"axis_map must have 3 items, got {value!r}")
    normalized = []
    for p in parts:
        if p not in ("x", "y", "z"):
            raise ValueError(f"axis_map item must be x/y/z, got {p!r}")
        normalized.append(str(p))
    return (normalized[0], normalized[1], normalized[2])


def transform_config_from_dict(d: Optional[Dict[str, Any]]) -> TransformConfig:
    d = d or {}
    return TransformConfig(
        axis_map=_axis_map(d.get("axis_map")),
        invert_x=bool(d.get("invert_x", False)),
        invert_y=bool(d.get("invert_y", False)),
        invert_z=bool(d.get("invert_z", False)),
        rotation_deg=_tuple3(d.get("rotation_deg", d.get("rotation_euler_deg")), (0.0, 0.0, 0.0)),
        scale=float(d.get("scale", 1.0)),
        offset_m=_tuple3(d.get("offset_m"), (0.0, 0.0, 0.0)),
    )


def hmd_relative_config_from_dict(d: Optional[Dict[str, Any]]) -> HmdRelativeConfig:
    d = d or {}
    mode = str(d.get("mode", "add"))
    if mode not in ("add", "subtract", "none"):
        raise ValueError("hmd_relative.mode must be add/subtract/none")
    return HmdRelativeConfig(
        enabled=bool(d.get("enabled", False)),
        mode=mode,
        offset_m=_tuple3(d.get("offset_m"), (0.0, 0.0, 0.0)),
    )


def stream_config_from_dict(name: str, base: StreamConfig, d: Optional[Dict[str, Any]]) -> StreamConfig:
    d = d or {}
    return StreamConfig(
        enabled=bool(d.get("enabled", base.enabled)),
        registry=str(d.get("registry", base.registry)),
        stream=str(d.get("stream", base.stream or name)),
        stale_ms=float(d.get("stale_ms", base.stale_ms)),
        coordinate_transform=transform_config_from_dict(d.get("coordinate_transform")),
        hmd_relative=hmd_relative_config_from_dict(d.get("hmd_relative")),
    )


def load_config(path: Optional[str]) -> ViewerConfig:
    cfg = ViewerConfig()
    if not path:
        return cfg
    p = Path(os.path.expanduser(path))
    with p.open("r", encoding="utf-8") as f:
        if p.suffix.lower() in (".json",):
            raw = json.load(f)
        else:
            if yaml is None:
                raise RuntimeError("PyYAML is required for YAML configs. Install: sudo apt install python3-yaml")
            raw = yaml.safe_load(f) or {}
    raw = raw or {}
    viewer = raw.get("viewer", {})
    streams = raw.get("streams", {})
    cfg.refresh_hz = float(viewer.get("refresh_hz", cfg.refresh_hz))
    cfg.registry_poll_interval_s = float(viewer.get("registry_poll_interval_s", cfg.registry_poll_interval_s))
    cfg.autoscale = bool(viewer.get("autoscale", cfg.autoscale))
    cfg.x_lim = _tuple2(viewer.get("x_lim"), cfg.x_lim)
    cfg.y_lim = _tuple2(viewer.get("y_lim"), cfg.y_lim)
    cfg.z_lim = _tuple2(viewer.get("z_lim"), cfg.z_lim)
    cfg.hmd = stream_config_from_dict("hmd", cfg.hmd, streams.get("hmd"))
    cfg.hand_tracking_21_joint = stream_config_from_dict(
        "hand_tracking_21_joint",
        cfg.hand_tracking_21_joint,
        streams.get("hand_tracking_21_joint", streams.get("mercury_hand_tracker")))
    cfg.hand_skeleton26 = stream_config_from_dict("hand_skeleton26", cfg.hand_skeleton26, streams.get("hand_skeleton26"))
    cfg.body_trackers = stream_config_from_dict("body_trackers", cfg.body_trackers, streams.get("body_trackers"))
    return cfg


def _tuple2(value: Any, default: Tuple[float, float]) -> Tuple[float, float]:
    if value is None:
        return default
    if isinstance(value, (list, tuple)) and len(value) == 2:
        return (float(value[0]), float(value[1]))
    raise ValueError(f"expected 2-vector, got {value!r}")


def _rotate_xyz(x: float, y: float, z: float, rotation_deg: Tuple[float, float, float]) -> Tuple[float, float, float]:
    rx, ry, rz = (math.radians(float(v)) for v in rotation_deg)
    if rx:
        c, s = math.cos(rx), math.sin(rx)
        y, z = y * c - z * s, y * s + z * c
    if ry:
        c, s = math.cos(ry), math.sin(ry)
        x, z = x * c + z * s, -x * s + z * c
    if rz:
        c, s = math.cos(rz), math.sin(rz)
        x, y = x * c - y * s, x * s + y * c
    return x, y, z


def apply_coordinate_transform(p: Tuple[float, float, float], t: TransformConfig) -> Tuple[float, float, float]:
    src = {"x": p[0], "y": p[1], "z": p[2]}
    x = src[t.axis_map[0]]
    y = src[t.axis_map[1]]
    z = src[t.axis_map[2]]
    if t.invert_x:
        x = -x
    if t.invert_y:
        y = -y
    if t.invert_z:
        z = -z
    x, y, z = _rotate_xyz(x, y, z, t.rotation_deg)
    x = x * t.scale + t.offset_m[0]
    y = y * t.scale + t.offset_m[1]
    z = z * t.scale + t.offset_m[2]
    return (x, y, z)


def apply_hmd_relative(
    p: Tuple[float, float, float],
    rel: HmdRelativeConfig,
    hmd: Optional[Tuple[float, float, float]],
) -> Tuple[float, float, float]:
    x, y, z = p
    if rel.enabled and hmd is not None:
        if rel.mode == "add":
            x += hmd[0]
            y += hmd[1]
            z += hmd[2]
        elif rel.mode == "subtract":
            x -= hmd[0]
            y -= hmd[1]
            z -= hmd[2]
    x += rel.offset_m[0]
    y += rel.offset_m[1]
    z += rel.offset_m[2]
    return (x, y, z)


def transform_point(
    p: Tuple[float, float, float],
    cfg: StreamConfig,
    hmd: Optional[Tuple[float, float, float]],
) -> Tuple[float, float, float]:
    return apply_hmd_relative(apply_coordinate_transform(p, cfg.coordinate_transform), cfg.hmd_relative, hmd)


# ---------------------------------------------------------------------------
# SHM ring reader.
# ---------------------------------------------------------------------------


@dataclass
class StreamStatus:
    present: bool = False
    connected: bool = False
    sequence: int = 0
    age_ms: Optional[float] = None
    error: str = ""
    shm_name: str = ""
    format_name: str = ""


class ShmRingReader:
    def __init__(self, cfg: StreamConfig, expected_size: int, expected_format_names: Iterable[str]):
        self.cfg = cfg
        self.expected_size = expected_size
        self.expected_format_names = set(expected_format_names)
        self._registry_mtime: float = 0.0
        self._next_registry_poll = 0.0
        self._entry: Optional[Dict[str, Any]] = None
        self._shm_name = ""
        self._path: Optional[Path] = None
        self._fd: Optional[int] = None
        self._mmap: Optional[mmap.mmap] = None
        self._size: int = 0
        self.status = StreamStatus(format_name="")

    def close(self) -> None:
        if self._mmap is not None:
            try:
                self._mmap.close()
            except Exception:
                pass
            self._mmap = None
        if self._fd is not None:
            try:
                os.close(self._fd)
            except Exception:
                pass
            self._fd = None
        self._path = None
        self._size = 0

    def _registry_entry(self) -> Optional[Dict[str, Any]]:
        registry = Path(os.path.expanduser(self.cfg.registry))
        now = time.monotonic()
        if now < self._next_registry_poll and self._entry is not None:
            return self._entry
        self._next_registry_poll = now + 1.0
        if not registry.exists():
            self.status = StreamStatus(present=False, connected=False, error=f"registry missing: {registry}")
            return None
        try:
            st = registry.stat()
            if self._entry is not None and st.st_mtime == self._registry_mtime:
                return self._entry
            with registry.open("r", encoding="utf-8") as f:
                j = json.load(f)
            streams = j.get("streams", {}) if isinstance(j, dict) else {}
            entry = streams.get(self.cfg.stream)
            self._registry_mtime = st.st_mtime
            self._entry = entry if isinstance(entry, dict) else None
            if self._entry is None:
                self.status = StreamStatus(present=False, connected=False,
                                           error=f"stream missing: {self.cfg.stream}")
            return self._entry
        except Exception as exc:
            self.status = StreamStatus(present=False, connected=False, error=f"registry read failed: {exc}")
            return None

    @staticmethod
    def _shm_path(shm_name: str) -> Path:
        name = shm_name[1:] if shm_name.startswith("/") else shm_name
        if sys.platform.startswith("linux"):
            return Path("/dev/shm") / name
        # Placeholder for future Windows/macOS shared-memory implementation.
        # The ABI is platform-neutral, but the current viewer backend reads Linux POSIX SHM files.
        return Path("/dev/shm") / name

    def _attach_if_needed(self, entry: Dict[str, Any]) -> bool:
        shm_name = str(entry.get("shm_name", ""))
        format_name = str(entry.get("format_name", ""))
        self.status.format_name = format_name
        if self.expected_format_names and format_name and format_name not in self.expected_format_names:
            self.status = StreamStatus(present=True, connected=False, shm_name=shm_name,
                                       format_name=format_name,
                                       error=f"unexpected format: {format_name}")
            return False
        if not shm_name:
            self.status = StreamStatus(present=True, connected=False, error="registry entry has no shm_name")
            return False
        path = self._shm_path(shm_name)
        expected_total = int(entry.get("header_size", 4096)) + int(entry.get("slot_count", 0)) * int(entry.get("slot_stride", 0))
        if expected_total <= 0:
            expected_total = 0
        if self._mmap is not None and self._shm_name == shm_name and self._path == path:
            return True
        self.close()
        if not path.exists():
            self.status = StreamStatus(present=True, connected=False, shm_name=shm_name,
                                       format_name=format_name, error=f"shm missing: {path}")
            return False
        try:
            fd = os.open(path, os.O_RDONLY)
            size = os.fstat(fd).st_size
            if expected_total > 0 and size < min(expected_total, 4096 + self.expected_size):
                os.close(fd)
                self.status = StreamStatus(present=True, connected=False, shm_name=shm_name,
                                           format_name=format_name, error=f"shm too small: {size}")
                return False
            mm = mmap.mmap(fd, size, access=mmap.ACCESS_READ)
            self._fd = fd
            self._mmap = mm
            self._path = path
            self._size = size
            self._shm_name = shm_name
            return True
        except Exception as exc:
            self.close()
            self.status = StreamStatus(present=True, connected=False, shm_name=shm_name,
                                       format_name=format_name, error=f"shm attach failed: {exc}")
            return False

    def read_payload(self) -> Optional[bytes]:
        if not self.cfg.enabled:
            self.status = StreamStatus(present=False, connected=False, error="disabled")
            return None
        entry = self._registry_entry()
        if not entry:
            return None
        if not self._attach_if_needed(entry):
            return None
        assert self._mmap is not None
        try:
            header = RingHeaderV1.from_buffer_copy(self._mmap[:ctypes.sizeof(RingHeaderV1)])
            latest = int(header.latest_sequence)
            if latest == 0 or header.slot_count == 0:
                self.status = StreamStatus(present=True, connected=True, shm_name=self._shm_name,
                                           format_name=str(entry.get("format_name", "")),
                                           sequence=latest, error="no frames yet")
                return None
            slot_count = int(header.slot_count)
            slot_stride = int(header.slot_stride)
            slot_header_size = int(header.slot_header_size)
            header_size = int(header.header_size)
            payload_size = int(header.payload_size)
            if payload_size < self.expected_size:
                self.status = StreamStatus(present=True, connected=True, shm_name=self._shm_name,
                                           format_name=str(entry.get("format_name", "")),
                                           sequence=latest, error=f"payload too small: {payload_size}")
                return None
            slot_idx = (latest - 1) % slot_count
            slot_off = header_size + slot_idx * slot_stride
            if slot_off + slot_header_size + self.expected_size > self._size:
                self.status = StreamStatus(present=True, connected=False, shm_name=self._shm_name,
                                           format_name=str(entry.get("format_name", "")),
                                           sequence=latest, error="slot offset out of mmap range")
                return None
            slot_bytes = self._mmap[slot_off:slot_off + ctypes.sizeof(RingSlotHeaderV1)]
            slot = RingSlotHeaderV1.from_buffer_copy(slot_bytes)
            if slot.seq_begin != slot.seq_end or (slot.seq_begin & 1) != 0:
                # Writer is mid-update; skip this draw frame.
                self.status = StreamStatus(present=True, connected=True, shm_name=self._shm_name,
                                           format_name=str(entry.get("format_name", "")),
                                           sequence=latest, error="writer active")
                return None
            payload = self._mmap[slot_off + slot_header_size:slot_off + slot_header_size + self.expected_size]
            age_ms = None
            if slot.timestamp_ns:
                age_ms = max(0.0, (time.monotonic_ns() - int(slot.timestamp_ns)) / 1e6)
            stale = age_ms is not None and self.cfg.stale_ms > 0 and age_ms > self.cfg.stale_ms
            self.status = StreamStatus(present=True, connected=not stale, shm_name=self._shm_name,
                                       format_name=str(entry.get("format_name", "")), sequence=latest,
                                       age_ms=age_ms, error="stale" if stale else "")
            if stale:
                return None
            return bytes(payload)
        except Exception as exc:
            self.status = StreamStatus(present=True, connected=False, shm_name=self._shm_name,
                                       format_name=str(entry.get("format_name", "")), error=f"read failed: {exc}")
            return None

    def read_struct(self, struct_type: Any) -> Optional[Any]:
        payload = self.read_payload()
        if payload is None:
            return None
        try:
            return struct_type.from_buffer_copy(payload[:ctypes.sizeof(struct_type)])
        except Exception as exc:
            self.status.error = f"decode failed: {exc}"
            return None


# ---------------------------------------------------------------------------
# Stream-specific extraction.
# ---------------------------------------------------------------------------


@dataclass
class Point:
    label: str
    xyz: Tuple[float, float, float]
    color: str
    size: int


@dataclass
class FramePoints:
    hmd: Optional[Point] = None
    hand_tracking_21_joint: List[Point] = field(default_factory=list)
    skeleton26: List[Point] = field(default_factory=list)
    body: List[Point] = field(default_factory=list)
    statuses: Dict[str, StreamStatus] = field(default_factory=dict)


def _finite3(p: Tuple[float, float, float]) -> bool:
    return all(math.isfinite(v) for v in p)


def hmd_point_from_frame(frame: Any) -> Optional[Tuple[float, float, float]]:
    flags = int(getattr(frame, "flags", 0))
    if hasattr(frame, "source_flags"):
        valid = (flags & RUNTIME_HMD_FLAG_POSE_VALID) != 0
    else:
        valid = (flags & HMD_FLAG_POSE_VALID) != 0
    if not valid and int(getattr(frame, "tracking_status", 0)) not in (2,):
        return None
    p = (float(frame.px), float(frame.py), float(frame.pz))
    return p if _finite3(p) else None


def hand_side_point(side: HandSideF32V2) -> Optional[Tuple[float, float, float]]:
    if int(side.status) == 0:
        return None
    if (int(side.flags) & HAND_POSE_VALID) == 0 and float(side.confidence) <= 0:
        return None
    candidates = [
        (float(side.controller_px), float(side.controller_py), float(side.controller_pz)),
        (float(side.palm_px), float(side.palm_py), float(side.palm_pz)),
        (float(side.wrist_px), float(side.wrist_py), float(side.wrist_pz)),
    ]
    for p in candidates:
        if _finite3(p) and any(abs(v) > 1e-7 for v in p):
            return p
    return None


def skeleton26_side_point(side: HandSkeleton26SideF32V1) -> Optional[Tuple[float, float, float]]:
    if int(side.status) == 0:
        return None
    if (int(side.flags) & (HAND_SKELETON26_SIDE_POSE_VALID | HAND_SKELETON26_SIDE_JOINTS_VALID)) == 0 and float(side.confidence) <= 0:
        return None

    # Prefer OpenXR PALM then WRIST, because for this viewer we intentionally draw
    # hand input as a single debug point, not as full skeleton lines.
    for idx in (0, 1):
        if idx < int(side.joint_count):
            j = side.joints[idx]
            if int(j.flags) & (HAND_SKELETON26_JOINT_POSITION_VALID | HAND_SKELETON26_JOINT_TRACKED):
                p = (float(j.px), float(j.py), float(j.pz))
                if _finite3(p):
                    return p

    valid_points: List[Tuple[float, float, float]] = []
    for i in range(min(int(side.joint_count), HAND_SKELETON26_JOINT_COUNT)):
        j = side.joints[i]
        if int(j.flags) & (HAND_SKELETON26_JOINT_POSITION_VALID | HAND_SKELETON26_JOINT_TRACKED):
            p = (float(j.px), float(j.py), float(j.pz))
            if _finite3(p):
                valid_points.append(p)
    if not valid_points:
        return None
    return (
        sum(p[0] for p in valid_points) / len(valid_points),
        sum(p[1] for p in valid_points) / len(valid_points),
        sum(p[2] for p in valid_points) / len(valid_points),
    )


def tracker_label(t: BodyTrackerF32V1) -> str:
    raw_id = bytes(t.tracker_id).split(b"\0", 1)[0].decode("utf-8", errors="ignore")
    role = BODY_TRACKER_ROLES.get(int(t.role), f"role{int(t.role)}")
    if raw_id:
        return f"{role}:{raw_id}"
    return f"{role}:{int(t.tracker_index)}"


def body_tracker_points(frame: BodyTrackerSetFrameF32V1) -> List[Tuple[str, Tuple[float, float, float]]]:
    out: List[Tuple[str, Tuple[float, float, float]]] = []
    count = min(int(frame.tracker_count), BODY_TRACKER_MAX_TRACKERS)
    for i in range(count):
        t = frame.trackers[i]
        if int(t.status) not in (1, 2):
            continue
        flags = int(t.flags)
        if (flags & (BODY_TRACKER_FLAG_POSE_VALID | BODY_TRACKER_FLAG_POSITION_VALID)) == 0 and float(t.confidence) <= 0:
            continue
        p = (float(t.pose.px), float(t.pose.py), float(t.pose.pz))
        if _finite3(p):
            out.append((tracker_label(t), p))
    return out


# ---------------------------------------------------------------------------
# Viewer loop.
# ---------------------------------------------------------------------------


class RuntimeDebugViewer:
    def __init__(self, cfg: ViewerConfig):
        self.cfg = cfg
        self.hmd_reader = ShmRingReader(
            cfg.hmd,
            expected_size=ctypes.sizeof(RuntimeHmdPoseF64V1),
            expected_format_names=(RUNTIME_HMD_POSE_FORMAT_NAME, HMD_POSE_FORMAT_NAME),
        )
        self.hand21_reader = ShmRingReader(
            cfg.hand_tracking_21_joint,
            expected_size=ctypes.sizeof(HandTrackingFrameF32V2),
            expected_format_names=HAND_TRACKING_21_JOINT_FORMAT_NAMES,
        )
        self.skeleton_reader = ShmRingReader(
            cfg.hand_skeleton26,
            expected_size=ctypes.sizeof(HandSkeleton26FrameF32V1),
            expected_format_names=(HAND_SKELETON26_FORMAT_NAME,),
        )
        self.body_reader = ShmRingReader(
            cfg.body_trackers,
            expected_size=ctypes.sizeof(BodyTrackerSetFrameF32V1),
            expected_format_names=(BODY_TRACKER_SET_FORMAT_NAME,),
        )

    def close(self) -> None:
        for r in (self.hmd_reader, self.hand21_reader, self.skeleton_reader, self.body_reader):
            r.close()

    def read_points(self) -> FramePoints:
        points = FramePoints()

        hmd_raw: Optional[Tuple[float, float, float]] = None
        hmd_frame_payload = self.hmd_reader.read_payload()
        if hmd_frame_payload is not None:
            # Dispatch between runtime_hmd_pose and source hmd_pose by payload size.
            if len(hmd_frame_payload) >= ctypes.sizeof(RuntimeHmdPoseF64V1):
                frame = RuntimeHmdPoseF64V1.from_buffer_copy(hmd_frame_payload[:ctypes.sizeof(RuntimeHmdPoseF64V1)])
                if int(frame.size_bytes) == ctypes.sizeof(RuntimeHmdPoseF64V1):
                    hmd_raw = hmd_point_from_frame(frame)
                else:
                    source = HmdPoseF64V1.from_buffer_copy(hmd_frame_payload[:ctypes.sizeof(HmdPoseF64V1)])
                    hmd_raw = hmd_point_from_frame(source)
        hmd_transformed: Optional[Tuple[float, float, float]] = None
        if hmd_raw is not None:
            hmd_transformed = transform_point(hmd_raw, self.cfg.hmd, None)
            points.hmd = Point("HMD", hmd_transformed, "green", 110)

        hand21 = self.hand21_reader.read_struct(HandTrackingFrameF32V2)
        if hand21 is not None:
            left = hand_side_point(hand21.left)
            right = hand_side_point(hand21.right)
            if left is not None:
                points.hand_tracking_21_joint.append(Point("21-joint hand left", transform_point(left, self.cfg.hand_tracking_21_joint, hmd_transformed), "blue", 55))
            if right is not None:
                points.hand_tracking_21_joint.append(Point("21-joint hand right", transform_point(right, self.cfg.hand_tracking_21_joint, hmd_transformed), "blue", 55))

        sk = self.skeleton_reader.read_struct(HandSkeleton26FrameF32V1)
        if sk is not None:
            left = skeleton26_side_point(sk.left)
            right = skeleton26_side_point(sk.right)
            if left is not None:
                points.skeleton26.append(Point("26J left", transform_point(left, self.cfg.hand_skeleton26, hmd_transformed), "red", 55))
            if right is not None:
                points.skeleton26.append(Point("26J right", transform_point(right, self.cfg.hand_skeleton26, hmd_transformed), "red", 55))

        body = self.body_reader.read_struct(BodyTrackerSetFrameF32V1)
        if body is not None:
            for label, p in body_tracker_points(body):
                points.body.append(Point(label, transform_point(p, self.cfg.body_trackers, hmd_transformed), "saddlebrown", 28))

        points.statuses = {
            "hmd": self.hmd_reader.status,
            "21j": self.hand21_reader.status,
            "26j": self.skeleton_reader.status,
            "body": self.body_reader.status,
        }
        return points

    def run(self) -> None:
        plt.ion()
        fig, (ax_xz, ax_xy) = plt.subplots(1, 2, figsize=(13, 6))
        fig.canvas.manager.set_window_title("XR runtime debug viewer") if hasattr(fig.canvas, "manager") else None

        refresh_s = 1.0 / max(1.0, self.cfg.refresh_hz)
        try:
            while plt.fignum_exists(fig.number):
                frame = self.read_points()
                self._draw(fig, ax_xz, ax_xy, frame)
                plt.pause(refresh_s)
        finally:
            self.close()

    def _draw(self, fig: Any, ax_xz: Any, ax_xy: Any, frame: FramePoints) -> None:
        ax_xz.clear()
        ax_xy.clear()

        all_points: List[Point] = []
        if frame.hmd:
            all_points.append(frame.hmd)
        all_points.extend(frame.hand_tracking_21_joint)
        all_points.extend(frame.skeleton26)
        all_points.extend(frame.body)

        self._setup_axis(ax_xz, "X/Z top view", "X, m", "Z, m", self.cfg.x_lim, self.cfg.z_lim)
        self._setup_axis(ax_xy, "X/Y view", "X, m", "Y, m", self.cfg.x_lim, self.cfg.y_lim)

        if self.cfg.autoscale and all_points:
            xs = [p.xyz[0] for p in all_points]
            ys = [p.xyz[1] for p in all_points]
            zs = [p.xyz[2] for p in all_points]
            ax_xz.set_xlim(_lim_with_margin(xs, 0.5))
            ax_xz.set_ylim(_lim_with_margin(zs, 0.5))
            ax_xy.set_xlim(_lim_with_margin(xs, 0.5))
            ax_xy.set_ylim(_lim_with_margin(ys, 0.5))

        for p in all_points:
            x, y, z = p.xyz
            ax_xz.scatter([x], [z], c=p.color, s=p.size)
            ax_xy.scatter([x], [y], c=p.color, s=p.size)
            ax_xz.annotate(p.label, (x, z), textcoords="offset points", xytext=(5, 5), fontsize=8)
            ax_xy.annotate(p.label, (x, y), textcoords="offset points", xytext=(5, 5), fontsize=8)

        status_text = self._status_text(frame.statuses)
        fig.suptitle(status_text, fontsize=9)
        fig.canvas.draw_idle()

    @staticmethod
    def _setup_axis(ax: Any, title: str, xlabel: str, ylabel: str,
                    xlim: Tuple[float, float], ylim: Tuple[float, float]) -> None:
        ax.set_title(title)
        ax.set_xlabel(xlabel)
        ax.set_ylabel(ylabel)
        ax.grid(True, alpha=0.25)
        ax.axhline(0, color="black", linewidth=0.5, alpha=0.25)
        ax.axvline(0, color="black", linewidth=0.5, alpha=0.25)
        ax.set_xlim(xlim)
        ax.set_ylim(ylim)
        ax.set_aspect("equal", adjustable="box")

    @staticmethod
    def _status_text(statuses: Dict[str, StreamStatus]) -> str:
        chunks = []
        for name, st in statuses.items():
            if st.connected:
                age = "?" if st.age_ms is None else f"{st.age_ms:.0f}ms"
                chunks.append(f"{name}: ok seq={st.sequence} age={age}")
            elif st.present:
                chunks.append(f"{name}: no ({st.error})")
            else:
                chunks.append(f"{name}: missing")
        return " | ".join(chunks)


def _lim_with_margin(values: List[float], margin: float) -> Tuple[float, float]:
    lo = min(values) - margin
    hi = max(values) + margin
    if abs(hi - lo) < 0.1:
        lo -= 0.5
        hi += 0.5
    return (lo, hi)


def main() -> int:
    parser = argparse.ArgumentParser(description="XR runtime/debug SHM coordinate viewer")
    parser.add_argument("--config", default=None, help="YAML/JSON config path")
    parser.add_argument("--runtime-registry", default=None,
                        help="Override runtime registry for hmd/21-joint-hand/body default streams")
    parser.add_argument("--source-registry", default=None,
                        help="Override source registry for hand_skeleton26 default stream")
    parser.add_argument("--autoscale", action="store_true", help="Autoscale axes around visible points")
    args = parser.parse_args()

    cfg = load_config(args.config)
    if args.runtime_registry:
        cfg.hmd.registry = args.runtime_registry
        cfg.hand_tracking_21_joint.registry = args.runtime_registry
        cfg.body_trackers.registry = args.runtime_registry
    if args.source_registry:
        cfg.hand_skeleton26.registry = args.source_registry
    if args.autoscale:
        cfg.autoscale = True

    RuntimeDebugViewer(cfg).run()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
