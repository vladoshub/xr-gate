#!/usr/bin/env python3
"""
Debug/watch xr_runtime_adapter 21-joint hand tracking gesture stream from SHM.

Unlike the minimal watcher, this prints SHM ring diagnostics when no committed
frame exists yet, and can scan slots to distinguish:
  - adapter/publisher not running
  - stale/empty SHM object
  - wrong stream/shm name
  - committed frames present but latest_sequence is stale
"""

import argparse
import json
import mmap
import os
import struct
import sys
import time
from dataclasses import dataclass

RING_MAGIC = b"HTRKRG1\x00"
HAND_21_JOINT_FORMAT_NAMES = {"HAND_TRACKING_21_JOINT_F32_V2", "HAND_TRACKING_F32_V2", "HAND_TRACKING_V2"}
HAND_V2_FRAME_SIZE = 2200
HAND_V2_SIDE_SIZE = 1072
HAND_V2_FRAME_LEFT_OFF = 56
HAND_V2_FRAME_RIGHT_OFF = HAND_V2_FRAME_LEFT_OFF + HAND_V2_SIDE_SIZE

# TrackingRingHeaderV1 packed offsets.
HDR_MAGIC_OFF = 0
HDR_VERSION_OFF = 8
HDR_HEADER_SIZE_OFF = 12
HDR_SLOT_COUNT_OFF = 16
HDR_SLOT_STRIDE_OFF = 20
HDR_SLOT_HEADER_SIZE_OFF = 24
HDR_PAYLOAD_SIZE_OFF = 28
HDR_RESERVED0_OFF = 32
HDR_LATEST_SEQUENCE_OFF = 40

# TrackingSlotHeaderV1 packed offsets.
SLOT_SEQ_BEGIN_OFF = 0
SLOT_SEQ_END_OFF = 8
SLOT_TIMESTAMP_NS_OFF = 16
SLOT_SOURCE_TIMESTAMP_NS_OFF = 24
SLOT_PAYLOAD_SIZE_OFF = 32
SLOT_FLAGS_OFF = 36

# HandTrackingFrameF32V2 offsets.
FRAME_VERSION_OFF = 0
FRAME_SIZE_BYTES_OFF = 4
FRAME_SEQUENCE_OFF = 8
FRAME_TIMESTAMP_NS_OFF = 16
FRAME_SOURCE_TIMESTAMP_NS_OFF = 24
FRAME_RESET_COUNTER_OFF = 32
FRAME_STATUS_OFF = 40
FRAME_FLAGS_OFF = 44
FRAME_CONFIDENCE_OFF = 48
FRAME_HAND_COUNT_OFF = 52

# HandSideF32V2 offsets.
SIDE_HANDEDNESS_OFF = 0
SIDE_STATUS_OFF = 4
SIDE_FLAGS_OFF = 8
SIDE_CONFIDENCE_OFF = 12
SIDE_PINCH_STRENGTH_OFF = 124
SIDE_GRAB_STRENGTH_OFF = 128
SIDE_PINCH_ACTIVE_OFF = 132
SIDE_GRAB_ACTIVE_OFF = 136
SIDE_JOINT_COUNT_OFF = 140
SIDE_RESERVED0_OFF = 144

HAND_POSE_VALID = 1 << 0
HAND_JOINTS_VALID = 1 << 3
HAND_PINCH_VALID = 1 << 4
HAND_GRAB_VALID = 1 << 5

BTN_TRIGGER = 1 << 0
BTN_GRIP = 1 << 1
BTN_MENU = 1 << 2
BTN_A = 1 << 3
BTN_B = 1 << 4
BTN_THUMBSTICK = 1 << 5
BUTTON_NAMES = [
    (BTN_TRIGGER, "trigger"),
    (BTN_GRIP, "grip"),
    (BTN_MENU, "menu"),
    (BTN_A, "A/thumbs_up"),
    (BTN_B, "B/victory"),
    (BTN_THUMBSTICK, "thumbstick_click"),
]

STATUS_NAMES = {0: "no_hands", 1: "tracking", 2: "lost", 3: "degraded"}


def u32(buf, off):
    return struct.unpack_from("<I", buf, off)[0]


def u64(buf, off):
    return struct.unpack_from("<Q", buf, off)[0]


def f32(buf, off):
    return struct.unpack_from("<f", buf, off)[0]


@dataclass
class StreamInfo:
    registry_path: str
    stream_id: str
    shm_name: str
    format_name: str
    format_version: int
    header_size: int
    slot_count: int
    slot_header_size: int
    slot_stride: int
    payload_size: int


def load_stream_info(registry_path: str, stream_id: str | None) -> StreamInfo:
    with open(registry_path, "r", encoding="utf-8") as f:
        registry = json.load(f)
    streams = registry.get("streams", {})
    if not streams:
        raise RuntimeError(f"no streams in registry {registry_path}")

    if stream_id is None:
        candidates = [k for k, v in streams.items() if v.get("format_name") in HAND_21_JOINT_FORMAT_NAMES]
        if not candidates:
            known = ", ".join(f"{k}:{v.get('format_name')}" for k, v in sorted(streams.items()))
            raise RuntimeError(f"no 21-joint hand tracking stream found in {registry_path}; streams: {known}")
        stream_id = candidates[0]
        print(f"[auto] using stream={stream_id}", file=sys.stderr)

    if stream_id not in streams:
        known = ", ".join(sorted(streams.keys())) or "<none>"
        raise RuntimeError(f"stream {stream_id!r} not found in {registry_path}; known streams: {known}")

    s = streams[stream_id]
    return StreamInfo(
        registry_path=registry_path,
        stream_id=stream_id,
        shm_name=s["shm_name"],
        format_name=s.get("format_name", ""),
        format_version=int(s.get("format_version", 0)),
        header_size=int(s.get("header_size", 4096)),
        slot_count=int(s.get("slot_count", 0)),
        slot_header_size=int(s.get("slot_header_size", 128)),
        slot_stride=int(s.get("slot_stride", 0)),
        payload_size=int(s.get("payload_size", 0)),
    )


def shm_path_from_name(shm_name: str) -> str:
    name = shm_name[1:] if shm_name.startswith("/") else shm_name
    return os.path.join("/dev/shm", name)


def decode_buttons(mask: int) -> str:
    names = [name for bit, name in BUTTON_NAMES if mask & bit]
    return ",".join(names) if names else "-"


def decode_side(payload: bytes, off: int):
    status = u32(payload, off + SIDE_STATUS_OFF)
    flags = u32(payload, off + SIDE_FLAGS_OFF)
    reserved0 = u32(payload, off + SIDE_RESERVED0_OFF)
    gestures = []
    if u32(payload, off + SIDE_PINCH_ACTIVE_OFF):
        gestures.append("PINCH")
    if u32(payload, off + SIDE_GRAB_ACTIVE_OFF):
        gestures.append("GRAB")
    if reserved0 & BTN_A:
        gestures.append("A/thumbs_up")
    if reserved0 & BTN_B:
        gestures.append("B/victory")
    if reserved0 & BTN_TRIGGER:
        gestures.append("trigger_btn")
    if reserved0 & BTN_GRIP:
        gestures.append("grip_btn")

    return {
        "status": status,
        "status_name": STATUS_NAMES.get(status, str(status)),
        "flags": flags,
        "confidence": f32(payload, off + SIDE_CONFIDENCE_OFF),
        "pinch_strength": f32(payload, off + SIDE_PINCH_STRENGTH_OFF),
        "grab_strength": f32(payload, off + SIDE_GRAB_STRENGTH_OFF),
        "pinch_active": u32(payload, off + SIDE_PINCH_ACTIVE_OFF),
        "grab_active": u32(payload, off + SIDE_GRAB_ACTIVE_OFF),
        "joint_count": u32(payload, off + SIDE_JOINT_COUNT_OFF),
        "reserved0": reserved0,
        "buttons": decode_buttons(reserved0),
        "gestures": "+".join(gestures) if gestures else "-",
    }


def slot_is_committed(mm: mmap.mmap, slot_base: int):
    seq_begin = u64(mm, slot_base + SLOT_SEQ_BEGIN_OFF)
    seq_end = u64(mm, slot_base + SLOT_SEQ_END_OFF)
    payload_size = u32(mm, slot_base + SLOT_PAYLOAD_SIZE_OFF)
    return seq_begin == seq_end and seq_begin != 0 and (seq_begin % 2) == 0 and payload_size > 0


def ring_header(mm: mmap.mmap):
    return {
        "magic": bytes(mm[0:8]),
        "version": u32(mm, HDR_VERSION_OFF),
        "header_size": u32(mm, HDR_HEADER_SIZE_OFF),
        "slot_count": u32(mm, HDR_SLOT_COUNT_OFF),
        "slot_stride": u32(mm, HDR_SLOT_STRIDE_OFF),
        "slot_header_size": u32(mm, HDR_SLOT_HEADER_SIZE_OFF),
        "payload_size": u32(mm, HDR_PAYLOAD_SIZE_OFF),
        "reserved0": u32(mm, HDR_RESERVED0_OFF),
        "latest_sequence": u64(mm, HDR_LATEST_SEQUENCE_OFF),
    }


def scan_committed_slots(mm: mmap.mmap, h: dict, limit: int | None = None):
    slot_count = h["slot_count"]
    if limit is not None:
        slot_count = min(slot_count, limit)
    out = []
    for i in range(slot_count):
        base = h["header_size"] + i * h["slot_stride"]
        if base + h["slot_header_size"] > len(mm):
            break
        seq_begin = u64(mm, base + SLOT_SEQ_BEGIN_OFF)
        seq_end = u64(mm, base + SLOT_SEQ_END_OFF)
        payload_size = u32(mm, base + SLOT_PAYLOAD_SIZE_OFF)
        if seq_begin or seq_end or payload_size:
            out.append((i, seq_begin, seq_end, payload_size, slot_is_committed(mm, base)))
    return out


def read_latest_payload(mm: mmap.mmap):
    h = ring_header(mm)
    if h["magic"] != RING_MAGIC:
        raise RuntimeError(f"bad ring magic: {h['magic']!r}; expected {RING_MAGIC!r}")
    latest_seq = h["latest_sequence"]
    if latest_seq == 0:
        return h, None, None

    slot_idx = (latest_seq - 1) % h["slot_count"]
    slot_base = h["header_size"] + slot_idx * h["slot_stride"]
    for _ in range(5):
        seq_begin_1 = u64(mm, slot_base + SLOT_SEQ_BEGIN_OFF)
        seq_end_1 = u64(mm, slot_base + SLOT_SEQ_END_OFF)
        payload_size = u32(mm, slot_base + SLOT_PAYLOAD_SIZE_OFF)
        payload_off = slot_base + h["slot_header_size"]
        payload = bytes(mm[payload_off:payload_off + payload_size])
        seq_begin_2 = u64(mm, slot_base + SLOT_SEQ_BEGIN_OFF)
        seq_end_2 = u64(mm, slot_base + SLOT_SEQ_END_OFF)
        if seq_begin_1 == seq_end_1 == seq_begin_2 == seq_end_2 and seq_begin_1 != 0 and (seq_begin_1 % 2) == 0:
            return h, latest_seq, payload
        time.sleep(0.001)
    return h, None, None


def read_payload_from_best_committed_slot(mm: mmap.mmap, h: dict):
    slots = scan_committed_slots(mm, h, None)
    committed = [s for s in slots if s[4]]
    if not committed:
        return None, None
    # committed seq is frame sequence * 2; pick max committed seq.
    best = max(committed, key=lambda s: s[1])
    i, seq_begin, _seq_end, payload_size, _ = best
    base = h["header_size"] + i * h["slot_stride"]
    payload_off = base + h["slot_header_size"]
    return seq_begin // 2, bytes(mm[payload_off:payload_off + payload_size])


def format_side(label: str, side: dict) -> str:
    valid = []
    if side["flags"] & HAND_POSE_VALID:
        valid.append("pose")
    if side["flags"] & HAND_JOINTS_VALID:
        valid.append("joints")
    if side["flags"] & HAND_PINCH_VALID:
        valid.append("pinch_valid")
    if side["flags"] & HAND_GRAB_VALID:
        valid.append("grab_valid")
    return (
        f"{label:<5} status={side['status_name']:<8} conf={side['confidence']:.2f} "
        f"pinch={side['pinch_strength']:.3f}/{side['pinch_active']} "
        f"grab={side['grab_strength']:.3f}/{side['grab_active']} "
        f"buttons=0x{side['reserved0']:02x}({side['buttons']}) "
        f"gesture={side['gestures']} flags=0x{side['flags']:02x}({','.join(valid) if valid else '-'})"
    )


def print_frame(info: StreamInfo, ring_seq, payload: bytes):
    frame_seq = u64(payload, FRAME_SEQUENCE_OFF)
    ts_ns = u64(payload, FRAME_TIMESTAMP_NS_OFF)
    source_ts_ns = u64(payload, FRAME_SOURCE_TIMESTAMP_NS_OFF)
    reset_counter = u64(payload, FRAME_RESET_COUNTER_OFF)
    tracking_status = u32(payload, FRAME_STATUS_OFF)
    frame_flags = u32(payload, FRAME_FLAGS_OFF)
    frame_conf = f32(payload, FRAME_CONFIDENCE_OFF)
    hand_count = u32(payload, FRAME_HAND_COUNT_OFF)
    age_ms = (time.monotonic_ns() - ts_ns) / 1_000_000.0 if ts_ns else 0.0
    source_age_ms = (time.monotonic_ns() - source_ts_ns) / 1_000_000.0 if source_ts_ns else 0.0
    left = decode_side(payload, HAND_V2_FRAME_LEFT_OFF)
    right = decode_side(payload, HAND_V2_FRAME_RIGHT_OFF)
    print(
        f"stream={info.stream_id} shm={info.shm_name} seq={frame_seq} ring_seq={ring_seq} "
        f"age={age_ms:.1f}ms source_age={source_age_ms:.1f}ms "
        f"status={STATUS_NAMES.get(tracking_status, tracking_status)} hands={hand_count} "
        f"frame_flags=0x{frame_flags:02x} conf={frame_conf:.2f} reset={reset_counter}"
    )
    print(format_side("LEFT", left))
    print(format_side("RIGHT", right))


def print_diag(info: StreamInfo, path: str, mm: mmap.mmap, h: dict):
    print("no committed frame yet")
    print(
        f"diag: registry={info.registry_path} stream={info.stream_id} "
        f"format={info.format_name}v{info.format_version} shm={info.shm_name} path={path}"
    )
    try:
        st = os.stat(path)
        print(f"diag: shm_size={st.st_size} bytes")
    except OSError as e:
        print(f"diag: stat failed: {e}")
    print(
        "diag: ring "
        f"magic={h['magic']!r} version={h['version']} header_size={h['header_size']} "
        f"slot_count={h['slot_count']} slot_stride={h['slot_stride']} "
        f"slot_header_size={h['slot_header_size']} payload_size={h['payload_size']} "
        f"reserved0={h['reserved0']} latest_sequence={h['latest_sequence']}"
    )
    slots = scan_committed_slots(mm, h, 16)
    if slots:
        print("diag: first non-empty slot headers:")
        for i, sb, se, ps, committed in slots[:8]:
            print(f"  slot[{i}] seq_begin={sb} seq_end={se} payload_size={ps} committed={committed}")
    else:
        print("diag: all first 16 slot headers are empty")
    print("hint: if latest_sequence stays 0, xr_runtime_adapter created the SHM stream but has not published frames yet.")
    print("hint: check adapter log for 'publish_runtime_hand_shm: true' and increasing 'runtime_hand_published'.")


def main():
    ap = argparse.ArgumentParser(description="Watch/diagnose xr_runtime_adapter runtime hand gestures from SHM")
    ap.add_argument("--registry", default="/tmp/runtime_tracking_streams.json")
    ap.add_argument("--stream", default="runtime_hand_tracking", help="stream id; use 'auto' to pick first 21-joint hand tracking stream")
    ap.add_argument("--rate", type=float, default=10.0)
    ap.add_argument("--once", action="store_true")
    ap.add_argument("--only-changes", action="store_true")
    ap.add_argument("--no-clear", action="store_true")
    ap.add_argument("--diag", action="store_true", help="print diagnostics when no committed frame exists")
    ap.add_argument("--scan-fallback", action="store_true", help="scan slots if latest_sequence is stale/zero")
    args = ap.parse_args()

    stream = None if args.stream == "auto" else args.stream
    info = load_stream_info(args.registry, stream)
    if info.format_name not in HAND_21_JOINT_FORMAT_NAMES or info.format_version != 2:
        raise RuntimeError(f"stream {info.stream_id!r} is {info.format_name} v{info.format_version}; expected 21-joint hand tracking v2")
    if info.payload_size != HAND_V2_FRAME_SIZE:
        print(f"warning: registry payload_size={info.payload_size}, expected {HAND_V2_FRAME_SIZE}", file=sys.stderr)

    path = shm_path_from_name(info.shm_name)
    with open(path, "rb", buffering=0) as f:
        mm = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
        try:
            last_frame_seq = None
            last_diag_t = 0.0
            interval = 1.0 / max(args.rate, 0.1)
            while True:
                h, ring_seq, payload = read_latest_payload(mm)
                if payload is None and args.scan_fallback:
                    ring_seq, payload = read_payload_from_best_committed_slot(mm, h)
                if payload is None:
                    if not args.only_changes:
                        now = time.monotonic()
                        if args.diag and now - last_diag_t > 1.0:
                            print_diag(info, path, mm, h)
                            last_diag_t = now
                        elif not args.diag:
                            print("no committed frame yet")
                    if args.once:
                        return 2
                    time.sleep(interval)
                    continue

                frame_seq = u64(payload, FRAME_SEQUENCE_OFF)
                if args.only_changes and frame_seq == last_frame_seq:
                    time.sleep(interval)
                    continue
                last_frame_seq = frame_seq

                if not args.no_clear and not args.once:
                    print("\033[2J\033[H", end="")
                print_frame(info, ring_seq, payload)
                sys.stdout.flush()
                if args.once:
                    return 0
                time.sleep(interval)
        finally:
            mm.close()


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except KeyboardInterrupt:
        raise SystemExit(130)
    except Exception as e:
        print(f"error: {e}", file=sys.stderr)
        raise SystemExit(1)
