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
import signal
import subprocess
import sys
import tempfile

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


def command(args, seconds=8, allow_timeout=False):
    env = dict(os.environ, LC_ALL="C", LANG="C")
    process = subprocess.Popen(args, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT, text=True, encoding="utf-8",
                               errors="replace", env=env, start_new_session=True)
    try:
        output, _ = process.communicate(timeout=seconds)
    except BaseException as error:
        try:
            os.killpg(process.pid, signal.SIGTERM)
            output, _ = process.communicate(timeout=1)
        except subprocess.TimeoutExpired:
            os.killpg(process.pid, signal.SIGKILL)
            output, _ = process.communicate()
        except ProcessLookupError:
            output, _ = process.communicate()
        if isinstance(error, subprocess.TimeoutExpired) and allow_timeout:
            return ANSI.sub("", output)
        if isinstance(error, subprocess.TimeoutExpired):
            raise RuntimeError("Bluetooth command timed out: " + " ".join(args)) from None
        raise
    output = ANSI.sub("", output)
    if process.returncode:
        raise RuntimeError(output.strip() or "Bluetooth command failed: " + " ".join(args))
    return output


def ctl(*args, seconds=8):
    return command(["bluetoothctl", "--timeout", str(max(1, seconds - 2)), *args], seconds)


def device_info(mac):
    text = ctl("info", mac)
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


def discover(config, seconds=8):
    if not re.search(r"^Controller\s+", ctl("list"), re.M):
        raise RuntimeError("No Bluetooth controller is ready. Attach the selected adapter first.")
    ctl("power", "on")
    try:
        command(["bluetoothctl", "--timeout", str(seconds), "scan", "on"], seconds + 2, allow_timeout=True)
    finally:
        ctl("scan", "off")
    devices = {}
    for line in ctl("devices").splitlines():
        match = re.match(r"^Device\s+((?:[0-9A-F]{2}:){5}[0-9A-F]{2})\s+(.+)$", line, re.I)
        if match:
            mac = match.group(1).upper()
            devices[mac] = {"address": mac, "name": match.group(2), "label": match.group(2) + " [" + mac + "]"}
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


def pair_and_save(config_path, template_path, left, right):
    choices = {"left": address(left), "right": address(right)}
    if not any(choices.values()):
        raise ValueError("Select at least one controller.")
    if choices["left"] and choices["left"] == choices["right"]:
        raise ValueError("Left and right must use different controllers.")
    config = read_config(config_path, template_path)
    template = read_config(template_path) if template_path else copy.deepcopy(config)
    selected = {side: device_info(mac) if mac else None for side, mac in choices.items()}
    updated = configure(config, template, selected)  # Validate before pairing or writing.
    for side, device in selected.items():
        if device is None:
            continue
        mac = device["address"]
        log("Connecting " + side + ": " + device["name"] + " [" + mac + "]")
        if not device.get("paired"):
            ctl("--agent", "NoInputNoOutput", "pair", mac, seconds=25)
            if not device_info(mac).get("paired"):
                raise RuntimeError("Pairing did not complete: " + mac + ". Put the controller in pairing mode and retry.")
        ctl("trust", mac)
        if not device_info(mac).get("connected"):
            ctl("connect", mac, seconds=20)
        final = device_info(mac)
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
    args = parser.parse_args(argv)
    if args.scan:
        result = discover(read_config(args.config, args.template), args.scan_seconds)
    else:
        if args.left is None and args.right is None:
            found = discover(read_config(args.config, args.template), args.scan_seconds)
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
        result = pair_and_save(args.config, args.template, args.left, args.right)
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
