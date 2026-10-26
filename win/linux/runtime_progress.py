#!/usr/bin/env python3
"""Read current-session controller calibration without changing runtime/TTY I/O."""
import argparse
import json
import os
from pathlib import Path
import re
import selectors
import sys


def imu_sides(config):
    sides = set()
    for device in config.get("devices", []):
        if device.get("backend") != "gearvr_ble" or device.get("enabled") is False:
            continue
        if not (device.get("phys") or device.get("uniq") or str(device.get("stable_hash") or "0").strip("0")):
            continue  # An unassigned template is not a selected controller.
        side = device.get("imu_side")
        if side in ("left", "right"):
            sides.add(side)
        else:
            sides.update(binding["side"] for binding in config.get("bindings", [])
                         if binding.get("device_id") == device.get("id") and binding.get("side") in ("left", "right"))
    return sides


class CalibrationProgress:
    def __init__(self, sides):
        self.sides = sides
        self.zeroed = set()
        self.aligned = set()
        self.translation_disabled = set()
        self.stage = ""

    def begin(self):
        if self.sides and not self.stage:
            self.stage = "zeroing"
            return [self.stage]
        return []

    def feed(self, line):
        events = []
        if not self.sides or self.stage == "complete":
            return events
        if "controller IMU state reset:" in line:
            self.zeroed.clear()
            self.aligned.clear()
        for side, flags in re.findall(r"\bcontroller_(left|right)_imu_data_flags=(\d+)\b", line):
            ready = int(flags) & 40 == 40 and f"controller_{side}_connected=yes" in line
            if ready:
                self.zeroed.add(side)
            else:
                self.zeroed.discard(side)
                self.aligned.discard(side)
        disabled = re.search(r"\[imu-calibration\] side=(left|right) yaw=\w+ translation=disabled\b", line)
        if disabled:
            self.translation_disabled.add(disabled[1])
        if "[imu-alignment]" in line:
            for side in self.sides:
                yaw = re.search(rf"\bcontroller_{side}_acceleration_yaw_valid=(yes|no)\b", line)
                translation = re.search(rf"\bcontroller_{side}_acceleration_translation_yaw_calibrated=(yes|no)\b", line)
                ready = yaw is not None and yaw[1] == "yes" and (
                    translation is None or translation[1] == "yes" or side in self.translation_disabled)
                if ready:
                    self.aligned.add(side)
                else:
                    self.aligned.discard(side)
        if self.sides <= self.zeroed and self.stage == "zeroing":
            self.stage = "optical"
            events.append(self.stage)
        if self.stage == "optical" and self.sides <= self.zeroed and self.sides <= self.aligned:
            self.stage = "complete"
            events.append(self.stage)
        return events


def read_json(path):
    try:
        return json.loads(path.read_text(encoding="utf-8-sig"))
    except (FileNotFoundError, json.JSONDecodeError):
        return {}


def follow(base, launch_token, source=sys.stdin, output=sys.stdout):
    config = read_json(base / "config/override_controller.json")
    monitor = CalibrationProgress(imu_sides(config))
    def emit(events):
        for event in events:
            print(json.dumps({"stage": event}), file=output, flush=True)
    emit(monitor.begin())
    path = base / "logs/xr_runtime_adapter.log"
    offset, inode, pending = 0, None, b""
    with selectors.DefaultSelector() as selector:
        selector.register(source, selectors.EVENT_READ)
        while True:
            if selector.select(0.15) and not os.read(source.fileno(), 4096):
                return
            session = read_json(base / "runtime-session.json")
            if session.get("launch_token") != launch_token or session.get("session_kind") != "runtime":
                continue
            try:
                stat = path.stat()
                if stat.st_mtime_ns < session.get("started_ns", 0):
                    continue  # Never accept the previous run's calibration log.
                if inode != stat.st_ino or stat.st_size < offset:
                    offset, pending, inode = 0, b"", stat.st_ino
                with path.open("rb") as stream:
                    stream.seek(offset)
                    chunk = stream.read(256 * 1024)
                    offset = stream.tell()
                lines = (pending + chunk).split(b"\n")
                pending = lines.pop()[-256 * 1024:]
                for line in lines:
                    emit(monitor.feed(line.decode("utf-8", errors="replace")))
            except FileNotFoundError:
                continue


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--base", required=True)
    parser.add_argument("--launch-token", required=True)
    args = parser.parse_args()
    base = Path(args.base).expanduser()
    if not base.is_absolute() or base.resolve() == Path("/") or not re.fullmatch(r"[a-f0-9]{32}", args.launch_token):
        raise ValueError("Invalid progress session.")
    follow(base, args.launch_token)


if __name__ == "__main__":
    main()
