#!/usr/bin/env python3
"""Discover Bluetooth names, pair selected sides and save controller identities.

Standard library only. Device button mappings and backend come from the selected
controller template/config; USB identities and product names are never guessed.
"""
from __future__ import annotations

import argparse
import copy
from datetime import datetime
import json
import os
from pathlib import Path
import re
import selectors
import subprocess
import sys
import tempfile
import time

from select_bluetooth_adapter import BlueZ, clean, find_adapter, print_adapters

MAC = re.compile(r"(?:[0-9A-F]{2}:){5}[0-9A-F]{2}", re.I)
ANSI = re.compile(r"\x1b\[[0-?]*[ -/]*[@-~]")
BINDING_GROUPS = ("bindings", "alternative_bindings", "hold_toggle_bindings", "alternative_hold_toggle_bindings")


def log(message):
    print("[Bluetooth controllers] " + message, file=sys.stderr, flush=True)


def address(value):
    if not value or value.lower() == "none":
        return None
    if MAC.fullmatch(value) is None:
        raise ValueError("Invalid Bluetooth MAC address: " + value)
    return value.upper()


def choose_adapter(selector=None, interactive=False):
    adapters = BlueZ(5).adapters()
    if selector:
        selected = find_adapter(adapters, selector)
    else:
        powered = [adapter for adapter in adapters if adapter.powered]
        if len(powered) == 1:
            selected = powered[0]
        elif not powered:
            raise RuntimeError("No powered Bluetooth adapter. Run select_bluetooth_adapter.py first.")
        elif not interactive:
            choices = ", ".join(f"{adapter.hci} {adapter.address}" for adapter in powered)
            raise RuntimeError("Multiple powered Bluetooth adapters: " + choices + ". Use --adapter MAC or XR_BLUETOOTH_ADAPTER.")
        else:
            print_adapters(powered)
            value = input("Bluetooth adapter number, MAC or hci index (Enter cancels): ").strip()
            if not value:
                raise KeyboardInterrupt
            if value.isdecimal() and 1 <= int(value) <= len(powered):
                selected = powered[int(value) - 1]
            else:
                selected = find_adapter(powered, value)
    if not selected.powered:
        raise RuntimeError(f"{selected.hci} is powered off. Enable it with select_bluetooth_adapter.py first.")
    log(f"Adapter: {selected.name} [{selected.hci}, {selected.address}]")
    return selected


class BluetoothSession:
    """Keep adapter selection, discovery and the pairing agent in one client.

    bluetoothctl's `select` affects only that process. Each operation reselects
    and verifies the same MAC before issuing any device command. Adapter power
    and the desktop's default pairing agent are never changed here.
    """
    ERROR = re.compile(
        r"(?:^|\n|# )((?:Failed\b|Invalid command\b|No default controller\b|"
        r"(?:Controller|Device) (?:[0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2} not available\b|"
        r"Agent registration failed\b)[^\n]*)")
    POWERED = r"(?m)^\s*Powered:\s+(?:yes|no)\s*$"

    def __init__(self, adapter):
        self.adapter = adapter
        self.process = None
        self.reader = None

    def __enter__(self):
        try:
            self.process = subprocess.Popen(
                ["bluetoothctl", "--agent", "NoInputNoOutput"], stdin=subprocess.PIPE,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, bufsize=0,
                env=dict(os.environ, LC_ALL="C", LANG="C", TERM="dumb"), start_new_session=True)
            self.reader = selectors.DefaultSelector()
            self.reader.register(self.process.stdout, selectors.EVENT_READ)
            self.ensure_selected()
            return self
        except BaseException:
            self.close()
            raise

    def __exit__(self, *_):
        self.close()

    def close(self):
        process = self.process
        if process is not None:
            try:
                if process.poll() is None:
                    try:
                        process.stdin.write(b"quit\n")
                        process.stdin.flush()
                    except (OSError, ValueError):
                        pass
                    try:
                        process.wait(timeout=2)
                    except subprocess.TimeoutExpired:
                        process.terminate()
                        try:
                            process.wait(timeout=1)
                        except subprocess.TimeoutExpired:
                            process.kill()
                            process.wait(timeout=1)
            finally:
                if self.reader is not None:
                    self.reader.close()
                process.stdin.close()
                process.stdout.close()
                self.process = None

    def exchange(self, lines, finish, seconds=8, collect=False):
        # Discard leftover prompts/properties before starting a new request.
        if lines is not None:
            for _ in range(32):
                if not self.reader.select(0):
                    break
                if not os.read(self.process.stdout.fileno(), 65536):
                    raise RuntimeError("bluetoothctl closed unexpectedly.")
            try:
                self.process.stdin.write((lines + "\n").encode("ascii"))
                self.process.stdin.flush()
            except OSError:
                raise RuntimeError("bluetoothctl closed unexpectedly.") from None
        deadline = time.monotonic() + seconds
        output = bytearray()
        while True:
            text = ANSI.sub("", output.decode("utf-8", errors="replace")).replace("\r", "\n")
            text = re.sub(r"(?m)^\[[^\]\n]*\]# ?", "", text)
            error = self.ERROR.search(text)
            if error:
                raise RuntimeError(clean(error.group(1)))
            if finish and re.search(finish, text):
                return text
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                if collect:
                    return text
                raise RuntimeError("Bluetooth command timed out: " + clean(lines or "scan"))
            if not self.reader.select(remaining):
                continue
            chunk = os.read(self.process.stdout.fileno(), 65536)
            if not chunk:
                raise RuntimeError("bluetoothctl closed unexpectedly.")
            output.extend(chunk)
            if len(output) > 1024 * 1024:
                raise RuntimeError("Unexpectedly large bluetoothctl output.")

    def ensure_selected(self):
        text = self.exchange(f"select {self.adapter.address}\nshow", self.POWERED)
        controllers = re.findall(r"(?m)^Controller ((?:[0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2})\b", text)
        if not controllers or controllers[-1].upper() != self.adapter.address:
            raise RuntimeError("bluetoothctl did not select the requested adapter; no device command was sent.")
        if not re.search(r"(?m)^\s*Powered:\s+yes\s*$", text):
            raise RuntimeError("The selected Bluetooth adapter is powered off. Run select_bluetooth_adapter.py first.")

    def ctl(self, action, value=None, seconds=8):
        self.ensure_selected()
        if action == "devices" and value is None:
            # `show` is a synchronous delimiter even when the device list is empty.
            return self.exchange("devices\nshow", self.POWERED, seconds)
        if action == "scan" and value in ("on", "off"):
            state = "started" if value == "on" else "stopped"
            return self.exchange(f"scan {value}", rf"\bDiscovery {state}\b", seconds)
        mac = address(value)
        if mac is None or action not in ("info", "pair", "trust", "connect"):
            raise ValueError("Unsupported Bluetooth device command.")
        if action == "info":
            finish = rf"(?ms)^Device {re.escape(mac)}\b[^\n]*\n.*?^\s*Connected:\s+(?:yes|no)\s*$"
        elif action == "pair":
            self.exchange("agent NoInputNoOutput", r"Agent (?:is already )?registered", seconds)
            finish = r"\bPairing successful\b"
        elif action == "trust":
            finish = rf"Changing {re.escape(mac)} trust succeeded"
        else:
            finish = r"\bConnection successful\b"
        return self.exchange(f"{action} {mac}", finish, seconds)

    def scan(self, seconds):
        self.ctl("scan", "on")
        try:
            self.exchange(None, None, seconds, collect=True)
        finally:
            # Exiting this client also releases only its own discovery session.
            self.ctl("scan", "off")


def device_info(mac, session):
    text = session.ctl("info", mac)
    result = {"address": address(mac)}
    for key in ("Name", "Alias", "Paired", "Trusted", "Connected"):
        match = re.search(r"^\s*" + key + r":\s*(.*?)\s*$", text, re.M)
        if match:
            result[key.lower()] = match.group(1) if key in ("Name", "Alias") else match.group(1) == "yes"
    result["name"] = result.get("alias") or result.get("name")
    if not result.get("name"):
        raise RuntimeError("Bluetooth device has no readable Name: " + mac)
    return result


def read_config(path, template=None):
    source = path if path.is_file() else template
    if source is None or not source.is_file():
        raise FileNotFoundError("Select a controller template or import a controller config first.")
    value = json.loads(source.read_text(encoding="utf-8-sig"))
    if not isinstance(value, dict) or not isinstance(value.get("devices"), list):
        raise ValueError("Controller config must contain a devices list.")
    return value


def selected_sides(config):
    selected = {"left": None, "right": None}
    by_id = {device["id"]: device for device in config["devices"]}
    for side in selected:
        devices = [d for d in config["devices"] if d.get("imu_side") == side]
        if not devices:
            devices = [by_id[b["device_id"]] for b in config.get("bindings", [])
                       if b.get("side") == side and b.get("device_id") in by_id]
        for device in devices:
            if MAC.fullmatch(device.get("phys", "")):
                selected[side] = device["phys"].upper()
                break
    return selected


def discover(config, session, seconds=8):
    session.scan(seconds)
    devices = {}
    for line in session.ctl("devices").splitlines():
        match = re.match(r"^Device\s+((?:[0-9A-F]{2}:){5}[0-9A-F]{2})\s+(.+)$", line, re.I)
        if match:
            mac = match.group(1).upper()
            devices[mac] = {"address": mac, "name": match.group(2), "label": clean(match.group(2)) + " [" + mac + "]"}
    return {"devices": sorted(devices.values(), key=lambda x: (x["name"].casefold(), x["address"])),
            "selected": selected_sides(config)}


def fingerprint(device, backend):
    # Match gearvr_input_provider.cpp exactly. Other backend identity formats
    # require an implementation here instead of inheriting Gear VR assumptions.
    if backend != "gearvr_ble":
        raise ValueError("Automatic Bluetooth assignment is not implemented for backend " + backend + "; use controller training.")
    stable_id = backend + ":" + device["address"]
    material = "linux|" + backend + "|" + stable_id + "|" + device["name"]
    value = 1469598103934665603
    for byte in material.encode("utf-8"):
        value = ((value ^ byte) * 1099511628211) & ((1 << 64) - 1)
    return dict(platform="linux", backend=backend, name=device["name"], phys=device["address"],
                uniq=stable_id, stable_hash=f"{value:016x}", bustype=5, vendor=0,
                product=0, version=0, event_path="", by_id_path="", by_path="")


def configure(config, template, selected):
    if not any(selected.values()):
        raise ValueError("Select at least one controller.")
    values = [d["address"] for d in selected.values() if d]
    if len(values) != len(set(values)):
        raise ValueError("Left and right must use different Bluetooth devices.")
    result = copy.deepcopy(config)
    # IDs and calibration values are kept for existing sides. The template is
    # used only when adding a side which was previously omitted.
    for side, chosen in selected.items():
        existing = [d for d in result["devices"] if d.get("imu_side") == side]
        if not existing:
            ids = {b.get("device_id") for b in result.get("bindings", []) if b.get("side") == side}
            existing = [d for d in result["devices"] if d.get("id") in ids]
        if not chosen and not existing:
            continue
        reference = existing or [d for d in template["devices"] if d.get("imu_side") == side]
        if len(reference) != 1:
            raise ValueError("The profile must identify exactly one controller for the " + side + " side.")
        device = reference[0]
        backend = device.get("backend", "")
        if backend != "gearvr_ble":
            raise ValueError("Select a Bluetooth controller profile; automatic assignment does not support " + backend)
        if not chosen:
            ids = {d["id"] for d in existing}
            result["devices"] = [d for d in result["devices"] if d.get("id") not in ids]
            for group in BINDING_GROUPS:
                if group in result:
                    result[group] = [b for b in result[group] if b.get("side") != side]
            continue
        if existing:
            target = existing[0]
        else:
            target = copy.deepcopy(device)
            used = {d["id"] for d in result["devices"]}
            target["id"] = max(used | {0}) + 1
            result["devices"].append(target)
            for group in BINDING_GROUPS:
                for binding in template.get(group, []):
                    if binding.get("side") == side:
                        added = copy.deepcopy(binding)
                        added["device_id"] = target["id"]
                        result.setdefault(group, []).append(added)
        target.update(fingerprint(chosen, backend), imu_side=side)
        # Older configs may also embed a fingerprint in each binding.
        for group in BINDING_GROUPS:
            for binding in result.get(group, []):
                if binding.get("side") == side and binding.get("device_id") == target["id"] and "device" in binding:
                    binding["device"] = fingerprint(chosen, backend)
    # A disabled side cannot retain a toggle referencing a removed device ID.
    if result.get("layout_switch", {}).get("device_id") not in {d["id"] for d in result["devices"]}:
        result.setdefault("layout_switch", {})["enabled"] = False
    return result


def save_config(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    original = path.read_bytes() if path.exists() else None
    if original is not None:
        backup = path.with_name(path.stem + ".before-bluetooth-" + datetime.now().strftime("%Y%m%d-%H%M%S-%f") + path.suffix)
        backup.write_bytes(original)
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(dir=path.parent, prefix=path.name + ".new-", delete=False) as stream:
            temporary = Path(stream.name)
            stream.write((json.dumps(value, indent=2, ensure_ascii=False) + "\n").encode("utf-8"))
        if path.exists():
            temporary.chmod(path.stat().st_mode & 0o777)
        os.replace(temporary, path)
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def pair_and_save(config_path, template_path, left, right, session):
    choices = {"left": address(left), "right": address(right)}
    if not any(choices.values()):
        raise ValueError("Select at least one controller.")
    if choices["left"] and choices["left"] == choices["right"]:
        raise ValueError("Left and right must use different controllers.")
    config = read_config(config_path, template_path)
    template = read_config(template_path) if template_path else copy.deepcopy(config)
    selected = {side: device_info(mac, session) if mac else None for side, mac in choices.items()}
    updated = configure(config, template, selected)  # Validate before pairing or writing.
    for side, device in selected.items():
        if device is None:
            continue
        mac = device["address"]
        log("Connecting " + side + ": " + clean(device["name"]) + " [" + mac + "]")
        if not device.get("paired"):
            session.ctl("pair", mac, seconds=25)
            if not device_info(mac, session).get("paired"):
                raise RuntimeError("Pairing did not complete: " + mac + ". Put the controller in pairing mode and retry.")
        session.ctl("trust", mac)
        if not device_info(mac, session).get("connected"):
            session.ctl("connect", mac, seconds=20)
        final = device_info(mac, session)
        if not all(final.get(k) for k in ("paired", "trusted", "connected")):
            raise RuntimeError("Controller is not paired, trusted and connected: " + mac)
    save_config(config_path, updated)
    return {"selected": choices, "config": str(config_path)}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", type=Path, required=True)
    parser.add_argument("--template", type=Path)
    parser.add_argument("--scan", action="store_true")
    parser.add_argument("--scan-seconds", type=int, default=8, choices=range(1, 31))
    parser.add_argument("--left")
    parser.add_argument("--right")
    parser.add_argument("--adapter", default=os.environ.get("XR_BLUETOOTH_ADAPTER"), metavar="MAC|hciN",
                        help="use this powered adapter; otherwise use the only powered adapter or prompt")
    args = parser.parse_args(argv)
    config = read_config(args.config, args.template)
    interactive = sys.stdin.isatty() and not args.scan
    adapter = choose_adapter(args.adapter, interactive=interactive)
    with BluetoothSession(adapter) as session:
        if args.scan:
            result = discover(config, session, args.scan_seconds)
        else:
            if args.left is None and args.right is None:
                if not interactive:
                    raise ValueError("Interactive input is unavailable. Use --scan, then --left MAC and/or --right MAC.")
                found = discover(config, session, args.scan_seconds)
                if not found["devices"]:
                    raise RuntimeError("No devices found on the selected adapter. Put controllers in pairing mode and retry.")
                for index, device in enumerate(found["devices"], 1):
                    print(f"{index}: {device['label']}")
                def choose(side):
                    value = input(side + " controller number (Enter = not selected): ").strip()
                    if not value:
                        return "none"
                    index = int(value)
                    if not 1 <= index <= len(found["devices"]):
                        raise ValueError("Choose a listed controller number.")
                    return found["devices"][index - 1]["address"]
                args.left, args.right = choose("Left"), choose("Right")
            result = pair_and_save(args.config, args.template, args.left, args.right, session)
    result["adapter"] = {"address": adapter.address, "hci": adapter.hci, "name": adapter.name}
    print(json.dumps(result, ensure_ascii=False), flush=True)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except KeyboardInterrupt:
        raise SystemExit(130)
    except Exception as error:
        log("ERROR: " + str(error))
        raise SystemExit(1)
