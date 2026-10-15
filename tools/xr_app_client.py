#!/usr/bin/env python3
"""Minimal standard-library client for XR Gate's opt-in tracking TCP API."""

import argparse
import json
import socket
import sys


def read_message(stream):
    line = stream.readline(1024 * 1024 + 1)
    if not line:
        raise EOFError("XR Gate disconnected")
    if len(line) > 1024 * 1024 or not line.endswith(b"\n"):
        raise ValueError("invalid or oversized XR Gate JSON line")
    return json.loads(line)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=45810)
    parser.add_argument("--frames", type=int, default=0, help="stop after N frames; 0 streams until Ctrl+C")
    parser.add_argument("--json", action="store_true", help="print complete JSON messages")
    args = parser.parse_args()

    with socket.create_connection((args.host, args.port), timeout=5) as sock:
        with sock.makefile("rb") as stream:
            hello = read_message(stream)
            if hello.get("type") != "hello" or hello.get("protocol") != "xr_gate_app_v1":
                raise ValueError("unsupported XR Gate application protocol")
            print(json.dumps(hello) if args.json else
                  f"Connected: {hello['protocol']}, frame={hello['runtime_frame']}, max={hello['rate_hz']} Hz",
                  flush=True)
            sock.settimeout(None)
            count = 0
            while args.frames <= 0 or count < args.frames:
                message = read_message(stream)
                if message.get("type") != "frame":
                    continue
                if args.json:
                    print(json.dumps(message), flush=True)
                else:
                    hmd = message.get("hmd") or {}
                    hands = message.get("hands") or {}
                    controllers = message.get("controllers") or {}
                    left, right = controllers.get("left") or {}, controllers.get("right") or {}
                    print(f"seq={message['sequence']} hmd_valid={hmd.get('pose_valid', False)} "
                          f"position={hmd.get('position')} hands={hands.get('hand_count', 0)} "
                          f"buttons=L:{left.get('buttons', '0')} R:{right.get('buttons', '0')}", flush=True)
                count += 1


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        pass
    except (OSError, EOFError, ValueError, KeyError) as error:
        print(f"xr_app_client: {error}", file=sys.stderr)
        sys.exit(1)
