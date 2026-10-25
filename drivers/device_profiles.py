#!/usr/bin/env python3
"""Discover configured device profiles without executing their shell files."""
import argparse
import json
from pathlib import Path
import re


def device_id(value):
    value = value.strip().lower().replace("-", "_")
    if not re.fullmatch(r"[a-z0-9][a-z0-9_.]*", value) or value.endswith("."):
        raise ValueError("Invalid driver device identifier: " + value)
    return value


def configured_devices(root, driver):
    root = Path(root)
    found = set()
    if driver == "steamvr":
        base = root / "drivers/openvr_driver"
        if (base / "resources/settings/default.vrsettings").is_file():
            found.add("generic")
        for path in sorted((base / "devices").glob("*/settings/default.vrsettings")):
            name = path.parent.parent.name
            if device_id(name) != name:
                raise ValueError("Use a canonical device folder name: " + name)
            settings = json.loads(path.read_text(encoding="utf-8-sig"))
            if not isinstance(settings.get("xr_tracking"), dict):
                raise ValueError("Missing xr_tracking settings: " + str(path))
            found.add(name)
    elif driver == "monado":
        for path in sorted((root / "drivers/monado_driver/profiles").glob("*.env")):
            text = path.read_text(encoding="utf-8-sig")
            # Shared controller/input/transport files are not device profiles.
            if not re.search(r"(?m)^\s*(?:export\s+)?XR_MONADO_(?:EYE_|WINDOW_|DISPLAY_|FOV_|IPD_|LENS_|REFRESH_)[A-Z_]*=", text):
                continue
            if device_id(path.stem) != path.stem:
                raise ValueError("Use a canonical device profile name: " + path.stem)
            found.add(path.stem)
    else:
        raise ValueError("Unknown driver: " + driver)
    if not found:
        raise ValueError("No configured device profiles for " + driver)
    return sorted(found)


def select_devices(root, driver, requested=""):
    available = configured_devices(root, driver)
    selected = list(dict.fromkeys(device_id(x) for x in re.split(r"[\s,;]+", requested.strip()) if x))
    if not selected:
        return available
    missing = sorted(set(selected) - set(available))
    if missing:
        raise ValueError("No " + driver + " configuration for " + ", ".join(missing) +
                         ". Available devices: " + ", ".join(available))
    return selected


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--driver", choices=("steamvr", "monado"), required=True)
    parser.add_argument("--devices", default="", help="Optional space/comma-separated device identifiers")
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args()
    devices = select_devices(args.root, args.driver, args.devices)
    print(json.dumps(devices) if args.json else "\n".join(devices))


if __name__ == "__main__":
    main()
