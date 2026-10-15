#!/usr/bin/env bash
# Source from an active build session. The SDK is shared only within this run.
: "${XR_BUILD_SESSION_DIR:?run through with_build_session.sh}"
export XR_OPENCV_PREFIX="${XR_OPENCV_PREFIX:-$XR_BUILD_SESSION_DIR/opencv/install}"
_xr_opencv_env_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
bash "$_xr_opencv_env_dir/build_opencv.sh"
# OpenCV CMake exports are relocatable; pkg-config's prefix needs relocation.
if [[ -f "$XR_OPENCV_PREFIX/lib/pkgconfig/opencv4.pc" ]]; then
  python3 - "$XR_OPENCV_PREFIX" <<'PY'
import sys
from pathlib import Path
prefix = Path(sys.argv[1])
pc = prefix / "lib/pkgconfig/opencv4.pc"
pc.write_text("\n".join("prefix=" + str(prefix) if line.startswith("prefix=") else line
                        for line in pc.read_text().splitlines()) + "\n")
PY
fi
export OpenCV_ROOT="$XR_OPENCV_PREFIX"
export PKG_CONFIG_PATH="$XR_OPENCV_PREFIX/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
unset _xr_opencv_env_dir
