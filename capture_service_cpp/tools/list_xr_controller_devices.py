#!/usr/bin/env python3
"""List xr_controller_v1 hardware UIDs and their serial ports.

The firmware emits a periodic 32-byte XCID identity frame alongside unchanged
64-byte XCTL IMU frames. This tool listens for XCID without requiring
capture_service_cpp to be running.
"""
from __future__ import annotations

import argparse
import glob
import json
import os
from pathlib import Path
import select
import struct
import sys
import termios
import time
import zlib

IDENTITY_MAGIC = b"XCID"
IDENTITY_VERSION = 1
IDENTITY_PACKET_SIZE = 32
IDENTITY_CRC_OFFSET = 28
IDENTITY_UID_VALID = 0x01
DEVICE_UID_MAX_SIZE = 16


def canonical(path: str) -> str:
    try:
        return str(Path(path).resolve(strict=False))
    except OSError:
        return path


def candidate_ports(explicit: list[str]) -> list[str]:
    requested: list[str] = []
    requested.extend(explicit)
    if not explicit:
        requested.extend(sorted(glob.glob("/dev/serial/by-id/*")))
        requested.extend(sorted(glob.glob("/dev/ttyACM*")))

    result: list[str] = []
    seen: set[str] = set()
    for path in requested:
        key = canonical(path)
        if key in seen:
            continue
        seen.add(key)
        result.append(path)
    return result


def baud_constant(baud: int) -> int:
    name = f"B{baud}"
    value = getattr(termios, name, None)
    if value is None:
        raise ValueError(f"unsupported baud rate on this platform: {baud}")
    return int(value)


def open_serial(path: str, baud: int) -> int:
    fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    try:
        attrs = termios.tcgetattr(fd)
        attrs[0] = 0
        attrs[1] = 0
        attrs[2] = termios.CLOCAL | termios.CREAD | termios.CS8
        attrs[3] = 0
        attrs[4] = baud_constant(baud)
        attrs[5] = baud_constant(baud)
        attrs[6][termios.VMIN] = 0
        attrs[6][termios.VTIME] = 0
        termios.tcsetattr(fd, termios.TCSANOW, attrs)
        termios.tcflush(fd, termios.TCIFLUSH)
        return fd
    except Exception:
        os.close(fd)
        raise


def decode_identity(packet: bytes) -> str | None:
    if len(packet) != IDENTITY_PACKET_SIZE:
        return None
    if packet[:4] != IDENTITY_MAGIC or packet[4] != IDENTITY_VERSION:
        return None
    if struct.unpack_from("<H", packet, 6)[0] != IDENTITY_PACKET_SIZE:
        return None
    expected_crc = struct.unpack_from("<I", packet, IDENTITY_CRC_OFFSET)[0]
    actual_crc = zlib.crc32(packet[:IDENTITY_CRC_OFFSET]) & 0xFFFFFFFF
    if expected_crc != actual_crc:
        return None
    uid_size = packet[8]
    if uid_size > DEVICE_UID_MAX_SIZE:
        return None
    if not (packet[5] & IDENTITY_UID_VALID) or uid_size == 0:
        return ""
    return packet[12 : 12 + uid_size].hex()


def probe(path: str, baud: int, timeout: float) -> tuple[str | None, str | None]:
    try:
        fd = open_serial(path, baud)
    except (OSError, ValueError) as exc:
        return None, str(exc)

    buffer = bytearray()
    deadline = time.monotonic() + timeout
    try:
        while time.monotonic() < deadline:
            remaining = max(0.0, deadline - time.monotonic())
            readable, _, _ = select.select([fd], [], [], min(0.10, remaining))
            if not readable:
                continue
            try:
                chunk = os.read(fd, 4096)
            except BlockingIOError:
                continue
            if not chunk:
                continue
            buffer.extend(chunk)

            while True:
                offset = buffer.find(IDENTITY_MAGIC)
                if offset < 0:
                    if len(buffer) > 3:
                        del buffer[:-3]
                    break
                if offset:
                    del buffer[:offset]
                if len(buffer) < IDENTITY_PACKET_SIZE:
                    break
                packet = bytes(buffer[:IDENTITY_PACKET_SIZE])
                uid = decode_identity(packet)
                if uid is not None:
                    return uid, None
                del buffer[0]
    finally:
        os.close(fd)
    return None, "no valid XCID frame received"


def main() -> int:
    parser = argparse.ArgumentParser(
        description="List xr_controller_v1 device_uid and serial port pairs"
    )
    parser.add_argument(
        "--port",
        action="append",
        default=[],
        help="probe only this port; may be repeated",
    )
    parser.add_argument("--baud", type=int, default=230400)
    parser.add_argument(
        "--timeout",
        type=float,
        default=1.5,
        help="seconds to wait per port; XCID is normally emitted once per second",
    )
    parser.add_argument("--json", action="store_true")
    parser.add_argument(
        "--show-errors",
        action="store_true",
        help="also print inaccessible/non-xr_controller_v1 ports",
    )
    args = parser.parse_args()

    if os.name != "posix":
        parser.error("this standard-library implementation currently supports POSIX serial ports")
    if args.timeout <= 0:
        parser.error("--timeout must be greater than zero")

    rows: list[dict[str, object]] = []
    for port in candidate_ports(args.port):
        uid, error = probe(port, args.baud, args.timeout)
        if uid is not None:
            rows.append(
                {
                    "device_uid": uid or None,
                    "protocol_device_uid": uid or None,
                    "port": port,
                    "canonical_port": canonical(port),
                    "baud": args.baud,
                }
            )
        elif args.show_errors:
            rows.append(
                {
                    "device_uid": None,
                    "protocol_device_uid": None,
                    "port": port,
                    "canonical_port": canonical(port),
                    "baud": args.baud,
                    "error": error,
                }
            )

    if args.json:
        print(json.dumps(rows, indent=2))
    else:
        found = [row for row in rows if "error" not in row]
        for row in found:
            uid = row["device_uid"] or "<unavailable; use port fallback>"
            print(f"device_uid={uid} port={row['port']} canonical_port={row['canonical_port']}")
        if args.show_errors:
            for row in rows:
                if "error" in row:
                    print(
                        f"port={row['port']} error={row['error']}",
                        file=sys.stderr,
                    )
        if found:
            print()
            print("capture_service_cpp config example:")
            for row in found:
                if row["device_uid"]:
                    print(f"  protocol_device_uid: \"{row['device_uid']}\"")
                else:
                    print(f"  port: \"{row['port']}\"")

    return 0 if any("error" not in row for row in rows) else 1


if __name__ == "__main__":
    raise SystemExit(main())
