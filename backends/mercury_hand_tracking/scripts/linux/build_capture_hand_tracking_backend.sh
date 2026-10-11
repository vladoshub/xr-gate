#!/usr/bin/env bash
set -euo pipefail

# Rebuild the XR Gate camera consumer without rebuilding Monado/Mercury,
# downloading models or changing calibration/runtime libraries.
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
ROOT_PROJECT="${ROOT_PROJECT:-$(cd -- "$SCRIPT_DIR/../../../.." && pwd)}"
BACKEND_DIR="${BACKEND_DIR:-$ROOT_PROJECT/backends/mercury_hand_tracking}"
BACKEND_BUILD_DIR="${BACKEND_BUILD_DIR:-$ROOT_PROJECT/build/backends/mercury_hand_tracking/backend}"
INSTALL_BIN_DIR="${INSTALL_BIN_DIR:-$ROOT_PROJECT/bin/backends/mercury_hand_tracking}"
CMAKE_BUILD_TYPE="${CMAKE_BUILD_TYPE:-RelWithDebInfo}"
BUILD_JOBS="${BUILD_JOBS:-$(nproc 2>/dev/null || echo 2)}"

configure_args=(-S "$BACKEND_DIR" -B "$BACKEND_BUILD_DIR"
  "-DCMAKE_BUILD_TYPE=$CMAKE_BUILD_TYPE" "-DXR_ROOT=$ROOT_PROJECT")
if [[ -n "${CLI11_INCLUDE_DIR:-}" ]]; then
  configure_args+=("-DCLI11_INCLUDE_DIR=$CLI11_INCLUDE_DIR")
fi
if [[ -n "${NLOHMANN_JSON_INCLUDE_DIR:-}" ]]; then
  configure_args+=("-DNLOHMANN_JSON_INCLUDE_DIR=$NLOHMANN_JSON_INCLUDE_DIR")
fi
cmake "${configure_args[@]}"
cmake --build "$BACKEND_BUILD_DIR" --target capture_hand_tracking_backend -j"$BUILD_JOBS"
mkdir -p "$INSTALL_BIN_DIR"
cp "$BACKEND_BUILD_DIR/capture_hand_tracking_backend" "$INSTALL_BIN_DIR/capture_hand_tracking_backend"
chmod +x "$INSTALL_BIN_DIR/capture_hand_tracking_backend"
printf '[OK] %s\n' "$INSTALL_BIN_DIR/capture_hand_tracking_backend"
