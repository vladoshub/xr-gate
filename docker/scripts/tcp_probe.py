#!/usr/bin/env python3
"""Check the application's JSON Lines protocol, without enabling video."""
import argparse
import json
import socket


def probe(host, port, timeout=8):
    with socket.create_connection((host, port), timeout=timeout) as connection:
        connection.settimeout(timeout)
        with connection.makefile("rb") as stream:
            hello = json.loads(stream.readline(1024 * 1024))
            if hello.get("type") != "hello" or hello.get("protocol") != "xr_gate_app_v1":
                raise ValueError(f"Unexpected server hello: {hello}")
            frame = json.loads(stream.readline(1024 * 1024))
            if frame.get("type") != "frame":
                raise ValueError(f"Unexpected frame: {frame}")
            print(json.dumps(hello, ensure_ascii=False))
            print(f'OK application TCP; frame sequence={frame.get("sequence", "?")}. '
                  'Receiving a frame does not prove valid tracking; inspect hmd/hands validity.')


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=45810)
    args = parser.parse_args()
    try:
        probe(args.host, args.port)
    except (OSError, ValueError) as error:
        parser.exit(1, f"TCP check failed: {error}\n")
