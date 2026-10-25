#!/usr/bin/env python3
"""Record real runtime 21-joint frames as labelled JSONL; standard library only."""

import argparse
import json
from pathlib import Path
import socket
import time


POSES = (
    ("neutral", "Curl all FOUR fingers, raise THUMB; hold wrist in its comfortable centre"),
    ("left", "Keep thumb raised, TILT THE WRIST for LEFT; do not steer with the thumb"),
    ("right", "Keep thumb raised, tilt the WRIST for RIGHT"),
    ("up", "Keep thumb raised, tilt WRIST FORWARD / AWAY from you (move character forward, +Y)"),
    ("down", "Keep thumb raised, tilt WRIST BACK / TOWARDS you (move character backward, -Y)"),
    ("thumb_rest", "Curl THUMB against palm and tilt wrist: pad must stay OFF"),
    ("open", "Straighten all four fingers; thumb may rest against palm (finger calibration)"),
    ("closed", "Curl all four fingers (finger calibration)"),
    ("trigger", "Curl middle/ring/little, EXTEND INDEX for trigger"),
    ("grip", "Curl index/middle/ring, EXTEND LITTLE for grip"),
    ("combo", "Curl middle/ring, extend INDEX + LITTLE + THUMB; tilt wrist"),
    ("button_a", "Make THUMB/INDEX ring with MIDDLE/RING/LITTLE CURLED"),
    ("button_b", "Make THUMB/INDEX ring with MIDDLE/RING/LITTLE STRAIGHT"),
    ("pad_click", "V sign: straighten INDEX + MIDDLE; curl RING + LITTLE; thumb position is free"),
)


def record(output, host, port, seconds, label, hand):
    count = 0
    valid_count = 0
    last_key = None
    with socket.create_connection((host, port), timeout=5) as sock:
        sock.settimeout(1)
        end = time.monotonic() + seconds
        last_message = time.monotonic()
        buffer = b""
        while time.monotonic() < end:
            try:
                chunk = sock.recv(65536)
            except socket.timeout:
                if time.monotonic() - last_message > 3:
                    raise RuntimeError("No frames for 3 seconds; check RUNTIME_APP_TCP and tracking")
                continue
            if not chunk:
                raise RuntimeError("Runtime disconnected; restart recording after tracking is running")
            buffer += chunk
            if len(buffer) > 2 * 1024 * 1024:
                raise RuntimeError("Runtime JSON line exceeds 2 MiB")
            while b"\n" in buffer:
                line, buffer = buffer.split(b"\n", 1)
                frame = json.loads(line)
                if frame.get("type") != "frame":
                    continue
                last_message = time.monotonic()
                hands = frame.get("hands") or {}
                key = (hands.get("source_timestamp_ns"), hands.get("reset_counter"),
                       (hands.get("left") or {}).get("status"),
                       (hands.get("right") or {}).get("status"))
                if key == last_key:
                    continue
                last_key = key
                frame.update(label=label, record_hand=hand)
                output.write(json.dumps(frame, separators=(",", ":"), allow_nan=False) + "\n")
                count += 1
                selected = ("left", "right") if hand == "both" else (hand,)
                if any((hands.get(name) or {}).get("status") == 1 and
                       len((hands.get(name) or {}).get("joints", [])) == 21 for name in selected):
                    valid_count += 1
    output.flush()
    print(f"{hand}.{label}: recorded {count} source frames ({valid_count} with 21 joints)", flush=True)
    if valid_count == 0:
        raise RuntimeError("No tracked 21-joint hand frames recorded; check hand tracking")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=45810)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--seconds", type=float, default=60)
    parser.add_argument("--calibrate", choices=("left", "right", "both"))
    parser.add_argument("--pose-seconds", type=float, default=3)
    parser.add_argument("--label", default="free")
    parser.add_argument("--append", action="store_true")
    args = parser.parse_args()
    if args.seconds <= 0 or args.pose_seconds <= 0:
        parser.error("durations must be positive")
    try:
        # Exclusive creation prevents accidentally destroying an earlier test.
        with args.output.open("a" if args.append else "x", encoding="utf-8") as output:
            if args.calibrate:
                for hand in (("left", "right") if args.calibrate == "both" else (args.calibrate,)):
                    for label, instruction in POSES:
                        if label != "neutral":
                            input(f"\n[{hand.upper()} / release] CURL all four fingers and tuck THUMB before the next pose. Press Enter: ")
                            record(output, args.host, args.port, 1, "release", hand)
                        input(f"\n[{hand.upper()} / {label}] {instruction}. Press Enter when ready: ")
                        record(output, args.host, args.port, args.pose_seconds, label, hand)
                input("\n[BOTH / menu] Show both OPEN palms, all fingers spread. Press Enter when ready: ")
                record(output, args.host, args.port, args.pose_seconds, "menu", "both")
            else:
                record(output, args.host, args.port, args.seconds, args.label, "both")
    except KeyboardInterrupt:
        print("\nRecording stopped; completed frames have been saved.")
    except (OSError, ValueError, RuntimeError) as exc:
        parser.exit(1, f"record_emulation_hands: {exc}\n")


if __name__ == "__main__":
    main()
