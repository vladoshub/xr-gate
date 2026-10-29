#!/usr/bin/env python3
"""Move legacy Gear VR left volume A/B bindings to X/Y in a saved profile."""

import argparse
from datetime import datetime
import json
import os
from pathlib import Path
import shutil
import tempfile


def migrate(config):
    backends = {device["id"]: device.get("backend") for device in config.get("devices", [])}
    changed = 0
    for group in ("bindings", "alternative_bindings", "hold_toggle_bindings",
                  "alternative_hold_toggle_bindings"):
        for binding in config.get(group, []):
            backend = backends.get(binding.get("device_id"), binding.get("device", {}).get("backend"))
            if (binding.get("side") != "left" or
                    backend not in ("gearvr", "gearvr_ble")):
                continue
            control = binding.get("input", {})
            if control.get("kind") != "key" or control.get("type") != 1:
                continue
            replacement = {(115, "a"): "x", (114, "b"): "y"}.get(
                (control.get("code"), binding.get("action")))
            if replacement:
                binding["action"] = replacement
                changed += 1
    return changed


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("config", type=Path, help="Existing Gear VR JSON profile")
    args = parser.parse_args()
    path = args.config.expanduser().resolve()
    try:
        config = json.loads(path.read_text(encoding="utf-8"))
        changed = migrate(config)
        if not changed:
            print("No legacy left Gear VR volume A/B bindings found; unchanged.")
            return
        contents = json.dumps(config, ensure_ascii=False, indent=2) + "\n"
        backup = path.with_name(path.name + ".before-xy-" + datetime.now().strftime("%Y%m%d-%H%M%S-%f"))
        shutil.copy2(path, backup)
        temporary = None
        try:
            with tempfile.NamedTemporaryFile(mode="w", encoding="utf-8", dir=path.parent,
                                             prefix="." + path.name + ".", delete=False) as output:
                temporary = Path(output.name)
                output.write(contents)
            os.chmod(temporary, path.stat().st_mode)
            os.replace(temporary, path)
        finally:
            if temporary is not None:
                temporary.unlink(missing_ok=True)
        print(f"Updated {changed} bindings in {path}; backup: {backup}")
    except (OSError, ValueError, TypeError, AttributeError, KeyError) as error:
        parser.exit(1, f"Cannot update {path}: {error}\n")


if __name__ == "__main__":
    main()
