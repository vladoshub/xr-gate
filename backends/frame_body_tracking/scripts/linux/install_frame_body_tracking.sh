#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_PROJECT="${ROOT_PROJECT:-$(cd "$SCRIPT_DIR/../../../.." && pwd)}"
# shellcheck source=/dev/null
source "$ROOT_PROJECT/devices/common/linux/scripts/build/dependency_policy.sh"

BIN_ROOT="${XR_BIN_ROOT:-$ROOT_PROJECT/bin}"
BUILD_DIR="${BUILD_DIR:-${XR_BUILD_ROOT:-$ROOT_PROJECT/build}/backends/frame_body_tracking}"
INSTALL_BIN_DIR="${INSTALL_BIN_DIR:-$BIN_ROOT/backends/frame_body_tracking}"
ORT_VERSION="${ORT_VERSION:-1.18.1}"
ORT_PACKAGE="onnxruntime-linux-x64-$ORT_VERSION"
ORT_ROOT="${ORT_ROOT:-$BIN_ROOT/onnxruntime/$ORT_PACKAGE}"
ORT_ARCHIVE="${ORT_ARCHIVE:-$BIN_ROOT/onnxruntime/$ORT_PACKAGE.tgz}"
ORT_URL="${ORT_URL:-https://github.com/microsoft/onnxruntime/releases/download/v$ORT_VERSION/$ORT_PACKAGE.tgz}"

if [[ ! -f "$ORT_ROOT/include/onnxruntime_cxx_api.h" || ! -f "$ORT_ROOT/lib/libonnxruntime.so" ]]; then
  xr_download "$ORT_URL" "$ORT_ARCHIVE"
  mkdir -p "$ORT_ROOT"
  tar -xzf "$ORT_ARCHIVE" --strip-components=1 -C "$ORT_ROOT"
fi

cmake -S "$ROOT_PROJECT/backends/frame_body_tracking" -B "$BUILD_DIR" \
  -DCMAKE_BUILD_TYPE="${CMAKE_BUILD_TYPE:-RelWithDebInfo}" \
  -DORT_ROOT="$ORT_ROOT"
cmake --build "$BUILD_DIR" --parallel "${BUILD_JOBS:-$(nproc)}"
cmake --install "$BUILD_DIR" --prefix "$INSTALL_BIN_DIR"
