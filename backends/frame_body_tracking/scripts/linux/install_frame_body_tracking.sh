#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_PROJECT="${ROOT_PROJECT:-$(cd "$SCRIPT_DIR/../../../.." && pwd)}"
# shellcheck source=/dev/null
source "$ROOT_PROJECT/devices/common/linux/scripts/build/dependency_policy.sh"

BIN_ROOT="${XR_BIN_ROOT:-$ROOT_PROJECT/bin}"
BUILD_DIR="${BUILD_DIR:-${XR_BUILD_ROOT:-$ROOT_PROJECT/build}/backends/frame_body_tracking}"
INSTALL_BIN_DIR="${INSTALL_BIN_DIR:-$BIN_ROOT/backends/frame_body_tracking}"
ORT_ROOT="${ORT_ROOT:-$(ROOT_PROJECT="$ROOT_PROJECT" bash "$ROOT_PROJECT/devices/common/linux/scripts/build/build_onnxruntime.sh")}"

cmake -S "$ROOT_PROJECT/backends/frame_body_tracking" -B "$BUILD_DIR" \
  -DCMAKE_BUILD_TYPE="${CMAKE_BUILD_TYPE:-RelWithDebInfo}" \
  -DORT_ROOT="$ORT_ROOT"
cmake --build "$BUILD_DIR" --parallel "${BUILD_JOBS:-$(nproc)}"
cmake --install "$BUILD_DIR" --prefix "$INSTALL_BIN_DIR"
