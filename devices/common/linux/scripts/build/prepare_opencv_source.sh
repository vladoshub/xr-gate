#!/usr/bin/env bash
# Keep the source checkout separate from per-invocation compiler/build caches.
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/dependency_policy.sh"
src="${1:?usage: prepare_opencv_source.sh source-dir commit [upstream]}"
commit="${2:?missing pinned OpenCV commit}"
upstream="${3:-https://github.com/opencv/opencv.git}"
mkdir -p "$src"
if [[ ! -e "$src/.git" ]]; then
  if [[ -n "$(find "$src" -mindepth 1 -maxdepth 1 -print -quit)" ]]; then
    echo "[opencv][ERROR] refusing a nonempty source directory without a Git checkout: $src" >&2
    exit 2
  fi
  git -C "$src" init -q
fi
if ! git -C "$src" remote get-url origin >/dev/null 2>&1; then
  git -C "$src" remote add origin "$upstream"
fi
if [[ "$XR_UPDATE_DEPS" == 1 ]] || ! git -C "$src" cat-file -e "$commit^{commit}" 2>/dev/null; then
  xr_require_network "OpenCV commit $commit missing or refresh requested: $src"
  git -C "$src" fetch --depth=1 origin "$commit"
fi
# Do not force checkout or clean: preserve any developer edits on conflict.
git -C "$src" checkout --detach "$commit"
[[ "$(git -C "$src" rev-parse HEAD)" == "$commit" ]]
# This generator is run by a separate `cmake -P` build command. Parent configure
# policy flags do not reach it; OpenCV 4.10 still requests CMake 2.8 in that path.
python3 - "$src/cmake/OpenCVGenPkgconfig.cmake" <<'PY'
import sys
from pathlib import Path

path = Path(sys.argv[1])
text = path.read_text()
old = "cmake_minimum_required(VERSION 2.8.12.2)"
new = "# XR Gate: CMake 4 compatibility for the script-mode pkg-config generator.\ncmake_minimum_required(VERSION 3.10)"
if text.count(old) == 1:
    path.write_text(text.replace(old, new, 1))
elif new not in text:
    raise SystemExit("Unrecognized OpenCV pkg-config generator; refusing to patch it")
PY
