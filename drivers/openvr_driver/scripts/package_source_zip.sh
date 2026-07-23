#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DRIVER_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
OUTPUT_ZIP="${1:-$(cd "$DRIVER_DIR/.." && pwd)/openvr_driver.zip}"

python3 - "$DRIVER_DIR" "$OUTPUT_ZIP" <<'PY'
from __future__ import annotations

import os
from pathlib import Path
import stat
import sys
import zipfile

source = Path(sys.argv[1]).resolve()
output = Path(sys.argv[2]).resolve()
output.parent.mkdir(parents=True, exist_ok=True)

excluded_names = {"__pycache__", ".git", "build", "out"}
excluded_suffixes = {".pyc", ".pyo", ".bak"}
files: list[Path] = []
for path in source.rglob("*"):
    relative = path.relative_to(source)
    if any(part in excluded_names for part in relative.parts):
        continue
    if path.is_file() and path.suffix not in excluded_suffixes:
        files.append(path)

with zipfile.ZipFile(output, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
    for path in sorted(files, key=lambda item: item.relative_to(source).as_posix()):
        relative = Path("openvr_driver") / path.relative_to(source)
        info = zipfile.ZipInfo(relative.as_posix(), date_time=(1980, 1, 1, 0, 0, 0))
        info.compress_type = zipfile.ZIP_DEFLATED
        info.create_system = 3
        mode = stat.S_IMODE(path.stat().st_mode)
        info.external_attr = (stat.S_IFREG | mode) << 16
        archive.writestr(info, path.read_bytes(), compress_type=zipfile.ZIP_DEFLATED, compresslevel=9)

print(f"[package_openvr_driver_source] wrote: {output}")
print(f"[package_openvr_driver_source] files: {len(files)}")
PY
