#!/usr/bin/env python3
"""Run one XR client session owned by a device's TCP connection."""
from __future__ import annotations

import argparse
import os
import re
import signal
import socket
import subprocess
import sys
import time
from pathlib import Path
from typing import Callable

from common import expand_path
from process_manager import terminate_one_process_tree

PROFILE_NAME = re.compile(rb"[A-Za-z0-9][A-Za-z0-9_-]{0,63}")
POLL_INTERVAL_S = 0.25
REQUEST_TIMEOUT_S = 5.0


def log(message: str) -> None:
    print(f"[xr_config_server] {message}", flush=True)


def read_profile_name(device: socket.socket) -> str:
    deadline = time.monotonic() + REQUEST_TIMEOUT_S
    request = bytearray()
    while len(request) < 66:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise TimeoutError("profile request timed out")
        device.settimeout(remaining)
        chunk = device.recv(66 - len(request))
        if not chunk:
            break
        request.extend(chunk)
        if b"\n" in request:
            name, _, trailing = request.partition(b"\n")
            name = name.removesuffix(b"\r")
            if not trailing and PROFILE_NAME.fullmatch(name):
                return name.decode("ascii")
            break
    raise ValueError("expected one profile name followed by a newline")


def find_profile(name: str, directories: list[Path]) -> Path:
    for directory in directories:
        candidate = directory / f"{name}.json"
        if candidate.is_file():
            return candidate
    raise ValueError(f"unknown profile {name}")


def reject(device: socket.socket, reason: str) -> None:
    try:
        device.sendall(f"ERR {reason}\n".encode("ascii"))
    except OSError:
        pass


def run_session(device: socket.socket, runner: Path, profile: Path, stopping: Callable[[], bool]) -> None:
    device.settimeout(POLL_INTERVAL_S)
    # Detect a device disappearing without sending a FIN, even with no
    # application traffic on the control connection.
    device.setsockopt(socket.SOL_SOCKET, socket.SO_KEEPALIVE, 1)
    device.setsockopt(socket.IPPROTO_TCP, socket.TCP_KEEPIDLE, 10)
    device.setsockopt(socket.IPPROTO_TCP, socket.TCP_KEEPINTVL, 3)
    device.setsockopt(socket.IPPROTO_TCP, socket.TCP_KEEPCNT, 3)

    try:
        process = subprocess.Popen(
            [str(runner), "--config", str(profile)],
            cwd=runner.parent,
            stdin=subprocess.DEVNULL,
            start_new_session=True,
        )
    except OSError as exc:
        log(f"Cannot start {profile.stem}: {exc}")
        reject(device, "cannot start launcher")
        return

    log(f"Started {profile.stem}")
    try:
        device.sendall(b"OK\n")
        while not stopping() and process.poll() is None:
            try:
                if not device.recv(1):
                    log("Device disconnected")
                else:
                    log("Unexpected data on the device control connection")
                break
            except socket.timeout:
                continue
    except OSError as exc:
        log(f"Device connection closed: {exc}")
    finally:
        terminate_one_process_tree(process, graceful_wait_s=15.0)
        log(f"Stopped {profile.stem} (launcher exit {process.returncode})")


def serve(runner: Path, host: str, port: int, directories: list[Path], stopping: Callable[[], bool]) -> None:
    while not stopping():
        session = None
        with socket.create_server((host, port)) as listener:
            log(f"Waiting for a device on {host}:{port}")
            listener.settimeout(POLL_INTERVAL_S)
            while not stopping():
                try:
                    connection, _ = listener.accept()
                except socket.timeout:
                    continue
                try:
                    profile = find_profile(read_profile_name(connection), directories)
                except (TimeoutError, ValueError) as exc:
                    reject(connection, str(exc))
                except OSError as exc:
                    log(f"Device connection closed: {exc}")
                else:
                    session = connection, profile
                    break
                connection.close()

        # Close the listening socket during the session. A second device
        # cannot queue a startup request while the current stack owns XR Gate.
        if session is not None:
            device, profile = session
            with device:
                if not stopping():
                    run_session(device, runner, profile, stopping)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, prog="run_xr_client.sh --config-server")
    parser.add_argument("runner", type=Path, help=argparse.SUPPRESS)
    parser.add_argument("--host", default="127.0.0.1", help="IPv4 listen address (default: %(default)s)")
    parser.add_argument("--port", type=int, default=45703, help="TCP listen port (default: %(default)s)")
    args = parser.parse_args()
    if not 1 <= args.port <= 65535:
        parser.error("--port must be between 1 and 65535")

    directories = [Path(__file__).resolve().parent / "configs"]
    if directory := os.environ.get("XR_CLIENT_CONFIG_DIR"):
        directories.insert(0, Path(expand_path(directory)).resolve())

    stop_requested = False

    def stop(_signum, _frame) -> None:
        nonlocal stop_requested
        stop_requested = True

    def stopping() -> bool:
        return stop_requested

    for signum in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP):
        signal.signal(signum, stop)

    try:
        serve(args.runner.resolve(), args.host, args.port, directories, stopping)
    except OSError as exc:
        log(f"Server error: {exc}")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
