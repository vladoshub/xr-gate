#!/usr/bin/env python3
"""Watch override_controller ControllerInputV2 SHM stream.

This is a debug tool for checking whether physical controller presses survive
through override_controller into the published controller_input stream.
It prints button/axis transitions, press durations, gaps, and counter deltas.
"""

from __future__ import annotations

import argparse
import csv
import json
import mmap
import os
import struct
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, Iterable, List, Optional, Tuple

BUTTONS = [
    (0, "trigger"),
    (1, "grip"),
    (2, "menu"),
    (3, "a"),
    (4, "b"),
    (5, "thumbstick"),
    (6, "dpad_up"),
    (7, "dpad_down"),
    (8, "dpad_left"),
    (9, "dpad_right"),
    (10, "dpad_center"),
    (11, "x"),
    (12, "y"),
    (13, "system"),
]
BIT_TO_NAME = {bit: name for bit, name in BUTTONS}
NAME_TO_BIT = {name: bit for bit, name in BUTTONS}

# Packed ABI from shared/include/xr_runtime/contracts/controller_input_contract.hpp
RING_HEADER_STRUCT = struct.Struct("<8s7IQ")  # 44 bytes with #pragma pack(1)
SLOT_HEADER_STRUCT = struct.Struct("<QQQQII")  # first 40 bytes of 128-byte slot header
FRAME_SIZE = 856
SIDE_SIZE = 400
FRAME_HEADER_SIZE = 56
LEFT_OFF = FRAME_HEADER_SIZE
RIGHT_OFF = FRAME_HEADER_SIZE + SIDE_SIZE

# ControllerDeviceStateV2 offsets inside each side payload.
SIDE_STATUS_OFF = 0
SIDE_SIDE_OFF = 4
SIDE_FLAGS_OFF = 8
SIDE_SOURCE_TYPE_OFF = 12
SIDE_BUTTONS_OFF = 16
SIDE_TOUCHES_OFF = 24
SIDE_CHANGED_BUTTONS_OFF = 32
SIDE_TRIGGER_OFF = 40
SIDE_GRIP_OFF = 44
SIDE_THUMBSTICK_X_OFF = 48
SIDE_THUMBSTICK_Y_OFF = 52
SIDE_STABLE_HASH_OFF = 64
SIDE_PHYSICAL_HASH_OFF = 72
SIDE_PRESS_COUNTERS_OFF = 80
SIDE_RELEASE_COUNTERS_OFF = 208
SIDE_DEVICE_ID_OFF = 336

STATUS_NAMES = {
    0: "unavailable",
    1: "connected",
    2: "active",
    3: "stale",
    4: "lost",
}

@dataclass
class RegistryInfo:
    shm_name: str
    header_size: int
    slot_count: int
    slot_stride: int
    slot_header_size: int
    payload_size: int

@dataclass
class SideState:
    status: int
    side: int
    flags: int
    source_type: int
    buttons: int
    touches: int
    changed_buttons: int
    trigger: float
    grip: float
    thumbstick_x: float
    thumbstick_y: float
    press_counters: Tuple[int, ...]
    release_counters: Tuple[int, ...]
    device_id: str

@dataclass
class Frame:
    version: int
    size_bytes: int
    sequence: int
    timestamp_ns: int
    source_timestamp_ns: int
    reset_counter: int
    flags: int
    active_mask: int
    connected_mask: int
    left: SideState
    right: SideState


def ns_to_ms(ns: int) -> float:
    return ns / 1_000_000.0


def bits_to_names(mask: int) -> List[str]:
    return [name for bit, name in BUTTONS if mask & (1 << bit)]


def parse_side(buf: bytes, base: int) -> SideState:
    status, side, flags, source_type = struct.unpack_from("<IIII", buf, base)
    buttons, touches, changed_buttons = struct.unpack_from("<QQQ", buf, base + SIDE_BUTTONS_OFF)
    trigger, grip, tx, ty = struct.unpack_from("<ffff", buf, base + SIDE_TRIGGER_OFF)
    press = struct.unpack_from("<32I", buf, base + SIDE_PRESS_COUNTERS_OFF)
    release = struct.unpack_from("<32I", buf, base + SIDE_RELEASE_COUNTERS_OFF)
    raw_id = buf[base + SIDE_DEVICE_ID_OFF:base + SIDE_DEVICE_ID_OFF + 64]
    device_id = raw_id.split(b"\0", 1)[0].decode("utf-8", "replace")
    return SideState(status, side, flags, source_type, buttons, touches, changed_buttons,
                     trigger, grip, tx, ty, press, release, device_id)


def parse_frame(payload: bytes) -> Frame:
    if len(payload) < FRAME_SIZE:
        raise ValueError(f"payload too small: {len(payload)} < {FRAME_SIZE}")
    version, size_bytes = struct.unpack_from("<II", payload, 0)
    sequence, timestamp_ns, source_timestamp_ns, reset_counter = struct.unpack_from("<QQQQ", payload, 8)
    flags, active_mask, connected_mask, _reserved0 = struct.unpack_from("<IIII", payload, 40)
    left = parse_side(payload, LEFT_OFF)
    right = parse_side(payload, RIGHT_OFF)
    return Frame(version, size_bytes, sequence, timestamp_ns, source_timestamp_ns,
                 reset_counter, flags, active_mask, connected_mask, left, right)


def read_registry(path: Path, stream: str) -> RegistryInfo:
    with path.open("r", encoding="utf-8") as f:
        data = json.load(f)
    streams = data.get("streams") or {}
    if stream not in streams:
        available = ", ".join(sorted(streams.keys()))
        raise RuntimeError(f"stream '{stream}' not found in {path}; available: {available}")
    s = streams[stream]
    return RegistryInfo(
        shm_name=str(s.get("shm_name", stream)),
        header_size=int(s.get("header_size", 4096)),
        slot_count=int(s.get("slot_count", 1024)),
        slot_stride=int(s.get("slot_stride", 128 + FRAME_SIZE)),
        slot_header_size=int(s.get("slot_header_size", 128)),
        payload_size=int(s.get("payload_size", FRAME_SIZE)),
    )


def shm_path_for_name(name: str) -> Path:
    return Path("/dev/shm") / name.lstrip("/")


def read_latest_sequence(mm: mmap.mmap) -> int:
    # Canonical packed RingHeaderV1 latest_sequence at offset 36; keep legacy
    # offset 40 compatibility for old buggy publishers/readers.
    latest = 0
    if len(mm) >= RING_HEADER_STRUCT.size:
        try:
            _magic, _ver, _hs, _sc, _ss, _shs, _ps, _r0, latest = RING_HEADER_STRUCT.unpack_from(mm, 0)
        except Exception:
            latest = 0
    if len(mm) >= 48:
        legacy = struct.unpack_from("<Q", mm, 40)[0]
        if legacy and legacy < (1 << 32) and (latest == 0 or latest > (1 << 32)):
            latest = legacy
    return latest


def read_frame(mm: mmap.mmap, info: RegistryInfo, expected_seq: Optional[int] = None) -> Optional[Frame]:
    latest = expected_seq if expected_seq is not None else read_latest_sequence(mm)
    if latest == 0:
        return None
    idx = (latest - 1) % info.slot_count
    off = info.header_size + idx * info.slot_stride
    if off + info.slot_header_size + info.payload_size > len(mm):
        return None

    seq_begin_1, seq_end_1, ts, source_ts, payload_size, flags = SLOT_HEADER_STRUCT.unpack_from(mm, off)
    if seq_begin_1 != seq_end_1 or (seq_begin_1 & 1):
        return None
    if payload_size < FRAME_SIZE:
        return None
    payload_off = off + info.slot_header_size
    payload = bytes(mm[payload_off:payload_off + FRAME_SIZE])
    seq_begin_2, seq_end_2, *_ = SLOT_HEADER_STRUCT.unpack_from(mm, off)
    if seq_begin_1 != seq_begin_2 or seq_end_1 != seq_end_2:
        return None
    try:
        frame = parse_frame(payload)
    except Exception:
        return None
    if frame.version != 2 or frame.size_bytes != FRAME_SIZE:
        return None
    return frame


def side_label(side_name: str, side: SideState) -> str:
    status = STATUS_NAMES.get(side.status, str(side.status))
    dev = f" dev={side.device_id}" if side.device_id else ""
    return f"{side_name} status={status} buttons={bits_to_names(side.buttons)} tx={side.thumbstick_x:+.2f} ty={side.thumbstick_y:+.2f}{dev}"


def iter_selected_bits(mask: Optional[str]) -> Iterable[int]:
    if not mask:
        return [bit for bit, _name in BUTTONS]
    out = []
    for token in mask.split(","):
        t = token.strip().lower().replace("-", "_")
        if not t:
            continue
        if t.isdigit():
            out.append(int(t))
        elif t in NAME_TO_BIT:
            out.append(NAME_TO_BIT[t])
        else:
            raise ValueError(f"unknown button filter: {token}")
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description="Watch override_controller controller_input SHM stream")
    ap.add_argument("--registry", default=os.environ.get("CONTROLLER_INPUT_REGISTRY", "/tmp/tracking_streams.json"))
    ap.add_argument("--stream", default=os.environ.get("CONTROLLER_INPUT_STREAM", "controller_input"))
    ap.add_argument("--duration-sec", type=float, default=30.0)
    ap.add_argument("--poll-ms", type=float, default=2.0)
    ap.add_argument("--buttons", default="", help="comma-separated button names/bits to print; default all")
    ap.add_argument("--print-every", type=float, default=1.0, help="periodic status line interval, seconds; 0 disables")
    ap.add_argument("--csv", default="", help="optional CSV output path")
    ap.add_argument("--raw", action="store_true", help="print every frame with active inputs")
    args = ap.parse_args()

    info = read_registry(Path(args.registry), args.stream)
    shm_path = shm_path_for_name(info.shm_name)
    if not shm_path.exists():
      raise RuntimeError(f"SHM not found: {shm_path}; is override_controller running?")

    selected_bits = set(iter_selected_bits(args.buttons))
    csv_file = None
    csv_writer = None
    if args.csv:
        csv_file = open(args.csv, "w", newline="", encoding="utf-8")
        csv_writer = csv.writer(csv_file)
        csv_writer.writerow(["wall_time", "seq", "side", "event", "button", "bit", "value", "dt_ms", "duration_ms", "buttons_mask", "touches_mask", "tx", "ty", "trigger", "grip", "press_counter", "release_counter", "device_id"])

    print(f"[watch_controller_input] registry={args.registry} stream={args.stream}")
    print(f"[watch_controller_input] shm={shm_path} header={info.header_size} slots={info.slot_count} stride={info.slot_stride} payload={info.payload_size}")
    print("[watch_controller_input] Press buttons now. Events show output after override_controller, not raw evdev.")

    last_frame: Optional[Frame] = None
    last_seq = 0
    last_seen_wall = time.monotonic()
    last_print_wall = 0.0
    press_start_ns: Dict[Tuple[str, int], int] = {}
    last_release_ns: Dict[Tuple[str, int], int] = {}

    def emit(frame: Frame, side_name: str, side: SideState, event: str, button: str = "", bit: int = -1,
             value: Optional[int] = None, dt_ms: Optional[float] = None, duration_ms: Optional[float] = None,
             press_counter: Optional[int] = None, release_counter: Optional[int] = None) -> None:
        t_ms = ns_to_ms(frame.timestamp_ns)
        parts = [f"seq={frame.sequence}", f"t={t_ms:.3f}ms", side_name, event]
        if button:
            parts.append(button)
        if value is not None:
            parts.append(f"value={value}")
        if dt_ms is not None:
            parts.append(f"dt={dt_ms:.1f}ms")
        if duration_ms is not None:
            parts.append(f"duration={duration_ms:.1f}ms")
        parts.append(f"buttons={bits_to_names(side.buttons)}")
        parts.append(f"tx={side.thumbstick_x:+.2f} ty={side.thumbstick_y:+.2f}")
        print("  ".join(parts), flush=True)
        if csv_writer:
            csv_writer.writerow([time.time(), frame.sequence, side_name, event, button, bit, value if value is not None else "", dt_ms if dt_ms is not None else "", duration_ms if duration_ms is not None else "", side.buttons, side.touches, side.thumbstick_x, side.thumbstick_y, side.trigger, side.grip, press_counter if press_counter is not None else "", release_counter if release_counter is not None else "", side.device_id])
            csv_file.flush()

    try:
        with shm_path.open("r+b") as f:
            mm = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
            start_wall = time.monotonic()
            while True:
                now = time.monotonic()
                if args.duration_sec > 0 and (now - start_wall) >= args.duration_sec:
                    break

                latest = read_latest_sequence(mm)
                if latest and latest != last_seq:
                    frame = read_frame(mm, info, latest)
                    if frame is None:
                        time.sleep(args.poll_ms / 1000.0)
                        continue
                    skipped = 0 if last_seq == 0 or frame.sequence <= last_seq + 1 else frame.sequence - last_seq - 1
                    if skipped:
                        print(f"[watch_controller_input] skipped {skipped} frames: last={last_seq} now={frame.sequence}")

                    if last_frame is None:
                        print(f"[initial] seq={frame.sequence} active_mask={frame.active_mask} connected_mask={frame.connected_mask}")
                        print("  " + side_label("left", frame.left))
                        print("  " + side_label("right", frame.right))
                    else:
                        dt_ms = ns_to_ms(frame.timestamp_ns - last_frame.timestamp_ns) if frame.timestamp_ns >= last_frame.timestamp_ns else None
                        for side_name, prev, cur in [("left", last_frame.left, frame.left), ("right", last_frame.right, frame.right)]:
                            changed = (prev.buttons ^ cur.buttons) | cur.changed_buttons
                            for bit in sorted(selected_bits):
                                bitmask = 1 << bit
                                name = BIT_TO_NAME.get(bit, str(bit))
                                prev_on = bool(prev.buttons & bitmask)
                                cur_on = bool(cur.buttons & bitmask)
                                key = (side_name, bit)
                                if prev.press_counters[bit] != cur.press_counters[bit]:
                                    emit(frame, side_name, cur, "PRESS_COUNTER", name, bit, 1, dt_ms, None, cur.press_counters[bit], cur.release_counters[bit])
                                if prev.release_counters[bit] != cur.release_counters[bit]:
                                    duration = None
                                    if key in press_start_ns:
                                        duration = ns_to_ms(frame.timestamp_ns - press_start_ns[key])
                                    emit(frame, side_name, cur, "RELEASE_COUNTER", name, bit, 0, dt_ms, duration, cur.press_counters[bit], cur.release_counters[bit])
                                if changed & bitmask or prev_on != cur_on:
                                    if cur_on and not prev_on:
                                        gap = None
                                        if key in last_release_ns:
                                            gap = ns_to_ms(frame.timestamp_ns - last_release_ns[key])
                                        press_start_ns[key] = frame.timestamp_ns
                                        emit(frame, side_name, cur, "PRESS", name, bit, 1, dt_ms, gap, cur.press_counters[bit], cur.release_counters[bit])
                                    elif prev_on and not cur_on:
                                        dur = None
                                        if key in press_start_ns:
                                            dur = ns_to_ms(frame.timestamp_ns - press_start_ns[key])
                                        last_release_ns[key] = frame.timestamp_ns
                                        emit(frame, side_name, cur, "RELEASE", name, bit, 0, dt_ms, dur, cur.press_counters[bit], cur.release_counters[bit])
                        # Print noticeable axis changes.
                        for side_name, prev, cur in [("left", last_frame.left, frame.left), ("right", last_frame.right, frame.right)]:
                            if (abs(cur.thumbstick_x - prev.thumbstick_x) >= 0.15 or
                                abs(cur.thumbstick_y - prev.thumbstick_y) >= 0.15 or
                                abs(cur.trigger - prev.trigger) >= 0.15 or
                                abs(cur.grip - prev.grip) >= 0.15):
                                emit(frame, side_name, cur, "AXIS", dt_ms=dt_ms)

                    if args.raw and (frame.left.buttons or frame.right.buttons or abs(frame.left.thumbstick_x) > 0.01 or abs(frame.left.thumbstick_y) > 0.01 or abs(frame.right.thumbstick_x) > 0.01 or abs(frame.right.thumbstick_y) > 0.01):
                        print(f"[raw] seq={frame.sequence} L={bits_to_names(frame.left.buttons)} lx={frame.left.thumbstick_x:+.2f} ly={frame.left.thumbstick_y:+.2f} R={bits_to_names(frame.right.buttons)} rx={frame.right.thumbstick_x:+.2f} ry={frame.right.thumbstick_y:+.2f}")

                    last_frame = frame
                    last_seq = frame.sequence
                    last_seen_wall = now

                if args.print_every > 0 and now - last_print_wall >= args.print_every:
                    if last_frame is not None:
                        age_ms = (now - last_seen_wall) * 1000.0
                        print(f"[status] seq={last_frame.sequence} age={age_ms:.1f}ms  {side_label('left', last_frame.left)}  {side_label('right', last_frame.right)}")
                    last_print_wall = now
                time.sleep(args.poll_ms / 1000.0)
    finally:
        if csv_file:
            csv_file.close()

    print("[watch_controller_input] done")
    return 0

if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except KeyboardInterrupt:
        print("\n[watch_controller_input] interrupted", file=sys.stderr)
        raise SystemExit(130)
    except Exception as e:
        print(f"[watch_controller_input][ERROR] {e}", file=sys.stderr)
        raise SystemExit(1)
