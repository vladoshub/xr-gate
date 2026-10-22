#!/usr/bin/env python3
"""Select a Linux Bluetooth adapter by hardware name, hci index or MAC.

Uses Python's standard library, BlueZ and systemd's busctl. USB/sysfs and
optional udev data supply hardware names; BlueZ names often just name the PC.
No pairing records, firmware, service configuration or XR Gate config is changed.
"""
from __future__ import annotations

import argparse
from dataclasses import dataclass
import json
import math
import os
from pathlib import Path
import re
import subprocess
import sys
import unicodedata


MAC = re.compile(r"(?:[0-9A-F]{2}:){5}[0-9A-F]{2}", re.I)
ADAPTER_PATH = re.compile(r"/org/bluez/hci[0-9]+")
INTERFACE = "org.bluez.Adapter1"


def clean(value: str) -> str:
    """Keep hardware descriptors and command errors safe for terminal output."""
    value = "".join(" " if unicodedata.category(c).startswith("C") else c for c in value)
    return " ".join(value.split())[:240]


def run_command(args: list[str], timeout: float) -> str:
    try:
        result = subprocess.run(
            args, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            text=True, encoding="utf-8", errors="replace", timeout=timeout,
            env=dict(os.environ, LC_ALL="C", LANG="C"), check=False,
        )
    except FileNotFoundError:
        raise RuntimeError(f"{args[0]} is not installed (install bluez and systemd).") from None
    except subprocess.TimeoutExpired:
        raise RuntimeError(f"{args[0]} timed out after {timeout:g}s.") from None
    if result.returncode:
        raise RuntimeError(clean(result.stderr or result.stdout) or f"{args[0]} failed.")
    return result.stdout


def property_value(output: str | dict, signature: str):
    """Decode busctl's typed JSON property/variant representation."""
    try:
        message = json.loads(output) if isinstance(output, str) else output
        if message["type"] != signature:
            raise ValueError("Unexpected property signature")
        value = message["data"]
        # Also accept a scalar representation of a single property.
        if isinstance(value, list):
            if len(value) != 1:
                raise ValueError("Expected one property value")
            value = value[0]
        if signature == "s" and isinstance(value, str):
            return value
        if signature == "b" and isinstance(value, bool):
            return value
    except (KeyError, TypeError, ValueError):
        pass
    raise RuntimeError("Invalid property response from busctl.")


@dataclass(frozen=True)
class Adapter:
    path: str
    address: str
    powered: bool
    name: str
    usb_id: str = ""

    @property
    def hci(self) -> str:
        return self.path.rsplit("/", 1)[-1]


def read_attribute(path: Path) -> str:
    try:
        return clean(path.read_text(encoding="utf-8", errors="replace"))
    except OSError:
        return ""


def hardware_name(hci: str, fallback: str, timeout: float,
                  sysfs: Path = Path("/sys/class/bluetooth")) -> tuple[str, str]:
    try:
        device = (sysfs / hci).resolve(strict=True)
    except OSError:
        return clean(fallback) or "Unknown model", ""
    usb = next((p for p in (device, *device.parents)
                if (p / "idVendor").is_file() and (p / "idProduct").is_file()), None)
    properties = {}
    try:
        output = run_command(["udevadm", "info", "--query=property", f"--path={usb or device}"], timeout)
        properties = dict(line.split("=", 1) for line in output.splitlines() if "=" in line)
    except RuntimeError:
        # Hardware descriptors still work on systems without udev's database/tool.
        pass
    model = (read_attribute(usb / "product") if usb else "")
    model = model or properties.get("ID_MODEL_FROM_DATABASE") or properties.get("ID_MODEL", "").replace("_", " ")
    vendor = (read_attribute(usb / "manufacturer") if usb else "")
    vendor = vendor or properties.get("ID_VENDOR_FROM_DATABASE") or properties.get("ID_VENDOR", "").replace("_", " ")
    parts = [clean(model)] if model else []
    db_model = clean(properties.get("ID_MODEL_FROM_DATABASE", ""))
    if db_model and all(db_model.casefold() not in part.casefold() for part in parts):
        parts.append(db_model)
    name = " / ".join(parts)
    if vendor and clean(vendor).casefold() not in name.casefold():
        name = clean(vendor) + (" " + name if name else "")
    usb_id = ""
    if usb:
        vid, pid = read_attribute(usb / "idVendor"), read_attribute(usb / "idProduct")
        if re.fullmatch(r"[0-9a-fA-F]{4}", vid) and re.fullmatch(r"[0-9a-fA-F]{4}", pid):
            usb_id = f"{vid}:{pid}".lower()
    return clean(name) or clean(fallback) or "Unknown model", usb_id


class BlueZ:
    def __init__(self, timeout: float):
        self.timeout = timeout

    def call(self, *args: str) -> str:
        return run_command(["busctl", "--system", "--no-pager", f"--timeout={self.timeout:g}s", *args], self.timeout + 1)

    def get(self, path: str, prop: str, signature: str):
        return property_value(self.call("--json=short", "get-property", "org.bluez", path, INTERFACE, prop), signature)

    def address(self, path: str) -> str:
        return self.valid_address(self.get(path, "Address", "s"), path)

    @staticmethod
    def valid_address(address: str, path: str) -> str:
        address = address.upper()
        if MAC.fullmatch(address) is None or address == "00:00:00:00:00:00":
            raise RuntimeError(f"{path} has no valid Bluetooth address.")
        return address

    def powered(self, path: str) -> bool:
        return self.get(path, "Powered", "b")

    def set_powered(self, path: str, powered: bool) -> None:
        self.call("set-property", "org.bluez", path, INTERFACE, "Powered", "b", str(powered).lower())

    def adapters(self) -> list[Adapter]:
        # One ObjectManager snapshot avoids recursively introspecting every
        # paired device, service and characteristic just to enumerate adapters.
        output = self.call("--json=short", "call", "org.bluez", "/",
                           "org.freedesktop.DBus.ObjectManager", "GetManagedObjects")
        try:
            message = json.loads(output)
            objects, = message["data"]
            if message["type"] != "a{oa{sa{sv}}}" or not isinstance(objects, dict):
                raise ValueError("Expected a managed object dictionary")
        except (KeyError, TypeError, ValueError):
            raise RuntimeError("Invalid BlueZ adapter inventory from busctl.") from None
        paths = []
        for path, interfaces in objects.items():
            if not ADAPTER_PATH.fullmatch(path):
                continue
            if not isinstance(interfaces, dict):
                raise RuntimeError("Invalid BlueZ adapter interfaces from busctl.")
            if INTERFACE in interfaces:
                paths.append(path)
        paths.sort(key=lambda path: int(path.rsplit("hci", 1)[1]))
        adapters = []
        for path in paths:
            properties = objects[path][INTERFACE]
            if not isinstance(properties, dict):
                raise RuntimeError("Invalid BlueZ adapter properties from busctl.")
            address = self.valid_address(property_value(properties.get("Address"), "s"), path)
            powered = property_value(properties.get("Powered"), "b")
            name = property_value(properties.get("Name", {"type": "s", "data": ""}), "s")
            name, usb_id = hardware_name(path.rsplit("/", 1)[-1], name, self.timeout)
            adapters.append(Adapter(path, address, powered, name, usb_id))
        return adapters


def find_adapter(adapters: list[Adapter], selector: str) -> Adapter:
    selector = selector.strip().casefold()
    matches = [a for a in adapters if selector in (a.hci.casefold(), a.address.casefold())]
    if len(matches) != 1:
        raise ValueError("Choose an adapter's MAC address or hci index from the list.")
    return matches[0]


def check_identity(bluez: BlueZ, adapter: Adapter) -> None:
    if bluez.address(adapter.path) != adapter.address:
        raise RuntimeError(f"{adapter.hci} changed identity. List adapters again before switching.")


class SwitchError(RuntimeError):
    def __init__(self, message: str, interrupted: bool = False):
        super().__init__(message)
        self.interrupted = interrupted


def switch_adapter(bluez: BlueZ, adapters: list[Adapter], selected: Adapter) -> None:
    # Refresh power states after the interactive prompt, before changing anything.
    original = {}
    for adapter in adapters:
        check_identity(bluez, adapter)
        original[adapter.path] = bluez.powered(adapter.path)
    attempted = []
    try:
        # A blocked/broken USB adapter must not first disconnect built-in devices.
        for adapter in [selected, *(a for a in adapters if a != selected)]:
            desired = adapter == selected
            check_identity(bluez, adapter)
            if bluez.powered(adapter.path) != desired:
                # A command may take effect even if the client times out.
                attempted.append(adapter)
                bluez.set_powered(adapter.path, desired)
            if bluez.powered(adapter.path) != desired:
                raise RuntimeError(f"{adapter.hci} did not become {'ON' if desired else 'OFF'}.")
        for adapter in adapters:
            check_identity(bluez, adapter)
            if bluez.powered(adapter.path) != (adapter == selected):
                raise RuntimeError(f"{adapter.hci} changed power state during switching.")
    except (Exception, KeyboardInterrupt) as error:
        failures = []
        # Restore originally enabled adapters first to recover connectivity.
        for adapter in sorted(attempted, key=lambda a: not original[a.path]):
            try:
                check_identity(bluez, adapter)
                bluez.set_powered(adapter.path, original[adapter.path])
                if bluez.powered(adapter.path) != original[adapter.path]:
                    raise RuntimeError("power state was not restored")
            except (Exception, KeyboardInterrupt) as restore_error:
                failures.append(f"{adapter.hci}: {clean(str(restore_error)) or 'interrupted'}")
        detail = "Original power states restored." if attempted else "No power states changed."
        if failures:
            detail = "Could not restore every adapter: " + "; ".join(failures)
        reason = clean(str(error)) or "Interrupted"
        raise SwitchError(f"{reason} {detail}", isinstance(error, KeyboardInterrupt)) from None


def print_adapters(adapters: list[Adapter]) -> None:
    print("Bluetooth adapters:")
    for number, adapter in enumerate(adapters, 1):
        print(f"  [{number}] {adapter.name}")
        usb = f" | USB {adapter.usb_id}" if adapter.usb_id else ""
        print(f"      {adapter.hci} | {adapter.address} | power: {'ON' if adapter.powered else 'OFF'}{usb}")


def timeout_value(value: str) -> float:
    try:
        seconds = float(value)
        if math.isfinite(seconds) and 0 < seconds <= 60:
            return seconds
    except ValueError:
        pass
    raise argparse.ArgumentTypeError("timeout must be between 0 (exclusive) and 60 seconds")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--list", action="store_true", help="list adapters without changing their power states")
    mode.add_argument("--adapter", default=os.environ.get("XR_BLUETOOTH_ADAPTER"), metavar="MAC|hciN",
                      help="enable this adapter and turn off the others (or set XR_BLUETOOTH_ADAPTER)")
    parser.add_argument("--timeout", type=timeout_value, default=os.environ.get("XR_BLUETOOTH_COMMAND_TIMEOUT", "5"),
                        help="timeout per command, in seconds (default: 5)")
    args = parser.parse_args(argv)
    try:
        if sys.platform != "linux":
            raise RuntimeError("Run this script inside Linux.")
        bluez = BlueZ(args.timeout)
        adapters = bluez.adapters()
        if not adapters:
            raise RuntimeError("No ready BlueZ adapters found. Check bluetooth.service, kernel support and firmware; USB visibility alone is not enough.")
        print_adapters(adapters)
        if args.list:
            return 0
        print("\nSwitching turns OFF the other Bluetooth adapters and disconnects their devices.")
        print("Stop XR Gate/controller capture before switching. Pairings stay on their original adapter.")
        if args.adapter:
            selected = find_adapter(adapters, args.adapter)
        else:
            if not sys.stdin.isatty():
                raise ValueError("Interactive input is unavailable. Use --list or --adapter MAC.")
            answer = input("Adapter number, MAC or hci index (Enter cancels): ").strip()
            if not answer:
                print("Cancelled; no power states changed.")
                return 0
            if answer.isdecimal() and 1 <= int(answer) <= len(adapters):
                selected = adapters[int(answer) - 1]
            else:
                selected = find_adapter(adapters, answer)
        switch_adapter(bluez, adapters, selected)
        print(f"\nSelected: {selected.name} | {selected.hci} | {selected.address} | power: ON")
        print("Other adapters: OFF. No reboot persistence is configured; rerun after reboot/hotplug if needed.")
        return 0
    except (RuntimeError, ValueError, EOFError) as error:
        message = clean(str(error)) or "No selection received."
        print("ERROR: " + message, file=sys.stderr)
        if re.search(r"denied|not.?authorized|authentication|permission", message, re.I):
            print("If authorization was denied, rerun this script with sudo.", file=sys.stderr)
        return 130 if isinstance(error, SwitchError) and error.interrupted else 1
    except KeyboardInterrupt:
        print("\nCancelled.", file=sys.stderr)
        return 130


if __name__ == "__main__":
    raise SystemExit(main())
