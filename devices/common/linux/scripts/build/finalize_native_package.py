#!/usr/bin/env python3
"""Strip package ELF files and reject accidental system OpenCV dependencies."""
import argparse
import shutil
import subprocess
from pathlib import Path


def finalize(root: Path, static_opencv: bool) -> int:
    for tool in ("readelf", "strip"):
        if not shutil.which(tool):
            raise SystemExit(f"{tool} is required to finalize native packages (install binutils)")
    count = 0
    for path in sorted((root / "bin").rglob("*")):
        # Python venvs and driver packages can contain links outside the package.
        if path.is_symlink() or not path.is_file():
            continue
        with path.open("rb") as stream:
            if stream.read(4) != b"\x7fELF":
                continue
        dynamic = subprocess.check_output(["readelf", "-d", str(path)], text=True)
        if static_opencv and any("(NEEDED)" in line and "libopencv_" in line
                                 for line in dynamic.splitlines()):
            raise SystemExit(f"System OpenCV leaked into {path}; rebuild with XR_OPENCV_PREFIX")
        subprocess.run(["strip", "--strip-unneeded", "--preserve-dates", str(path)], check=True)
        sections = subprocess.check_output(["readelf", "-SW", str(path)], text=True)
        if ".debug_info" in sections or ".zdebug_info" in sections:
            raise SystemExit(f"Debug information remains in {path}")
        count += 1
    print(f"[native-package] stripped {count} ELF files; static OpenCV required: {static_opencv}")
    return count


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("root", type=Path)
    parser.add_argument("--require-static-opencv", action="store_true")
    args = parser.parse_args()
    finalize(args.root, args.require_static_opencv)
