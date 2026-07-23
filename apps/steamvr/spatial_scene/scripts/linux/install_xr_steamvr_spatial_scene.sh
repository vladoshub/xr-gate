#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
APP_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
ROOT_PROJECT="${ROOT_PROJECT:-$(cd "$APP_DIR/../../.." && pwd)}"
BUILD_DIR="${BUILD_DIR:-$ROOT_PROJECT/build/apps/steamvr/spatial_scene/relwithdebinfo}"
INSTALL_BIN_DIR="${INSTALL_BIN_DIR:-$ROOT_PROJECT/bin/apps/steamvr/spatial_scene}"
XR_OPENVR_SDK_DIR="${XR_OPENVR_SDK_DIR:-$ROOT_PROJECT/third_party/openvr}"
BUILD_JOBS="${BUILD_JOBS:-$(nproc 2>/dev/null || echo 2)}"

if [[ ! -f "$XR_OPENVR_SDK_DIR/headers/openvr.h" && ! -f "$XR_OPENVR_SDK_DIR/src/headers/openvr.h" ]]; then
  echo "[install_xr_steamvr_spatial_scene][ERROR] OpenVR SDK not found at: $XR_OPENVR_SDK_DIR" >&2
  echo "[install_xr_steamvr_spatial_scene][ERROR] Set XR_OPENVR_SDK_DIR or fetch third_party/openvr first." >&2
  exit 2
fi

mkdir -p "$BUILD_DIR" "$INSTALL_BIN_DIR" "$INSTALL_BIN_DIR/lib"

echo "[install_xr_steamvr_spatial_scene] ROOT_PROJECT=$ROOT_PROJECT"
echo "[install_xr_steamvr_spatial_scene] APP_DIR=$APP_DIR"
echo "[install_xr_steamvr_spatial_scene] BUILD_DIR=$BUILD_DIR"
echo "[install_xr_steamvr_spatial_scene] INSTALL_BIN_DIR=$INSTALL_BIN_DIR"
echo "[install_xr_steamvr_spatial_scene] XR_OPENVR_SDK_DIR=$XR_OPENVR_SDK_DIR"

cmake -S "$APP_DIR" -B "$BUILD_DIR" \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DXR_OPENVR_SDK_DIR="$XR_OPENVR_SDK_DIR" \
  -DXR_SHARED_INCLUDE_DIR="$ROOT_PROJECT/shared/include"

cmake --build "$BUILD_DIR" --target xr_steamvr_spatial_scene -j"$BUILD_JOBS"

cp "$BUILD_DIR/xr_steamvr_spatial_scene" "$INSTALL_BIN_DIR/xr_steamvr_spatial_scene"

OPENVR_LIB=""
for p in \
  "$XR_OPENVR_SDK_DIR/lib/linux64/libopenvr_api.so" \
  "$XR_OPENVR_SDK_DIR/bin/linux64/libopenvr_api.so" \
  "$XR_OPENVR_SDK_DIR/lib/libopenvr_api.so"; do
  if [[ -f "$p" ]]; then OPENVR_LIB="$p"; break; fi
done
if [[ -n "$OPENVR_LIB" ]]; then
  cp "$OPENVR_LIB" "$INSTALL_BIN_DIR/lib/libopenvr_api.so"
fi

OPENVR_LICENSE_FILE=""
for candidate in "$XR_OPENVR_SDK_DIR/LICENSE" "$XR_OPENVR_SDK_DIR/LICENSE.txt"; do
  [[ -f "$candidate" ]] && { OPENVR_LICENSE_FILE="$candidate"; break; }
done
if [[ -z "$OPENVR_LICENSE_FILE" ]]; then
  echo "[install_xr_steamvr_spatial_scene][ERROR] OpenVR SDK license not found under: $XR_OPENVR_SDK_DIR" >&2
  exit 2
fi
mkdir -p "$INSTALL_BIN_DIR/licenses"
cp "$OPENVR_LICENSE_FILE" "$INSTALL_BIN_DIR/licenses/OpenVR-LICENSE.txt"
OPENVR_RESOLVED_COMMIT="unknown-external-checkout"
if [[ -d "$XR_OPENVR_SDK_DIR/.git" ]]; then
  OPENVR_RESOLVED_COMMIT="$(git -C "$XR_OPENVR_SDK_DIR" rev-parse HEAD)"
fi
cat > "$INSTALL_BIN_DIR/licenses/SOURCE_INFO.txt" <<EOF_OPENVR_APP_SOURCE
Component: Valve OpenVR SDK/runtime library used by apps/steamvr/spatial_scene/scripts/linux/install_xr_steamvr_spatial_scene.sh
License: BSD-3-Clause
SDK path: $XR_OPENVR_SDK_DIR
Resolved commit: $OPENVR_RESOLVED_COMMIT
EOF_OPENVR_APP_SOURCE

rm -rf "$INSTALL_BIN_DIR/scripts" "$INSTALL_BIN_DIR/configs"
cp -a "$APP_DIR/scripts" "$INSTALL_BIN_DIR/scripts"
cp -a "$APP_DIR/configs" "$INSTALL_BIN_DIR/configs"
cp "$APP_DIR/README.md" "$INSTALL_BIN_DIR/README.md"

ldd "$INSTALL_BIN_DIR/xr_steamvr_spatial_scene" > "$INSTALL_BIN_DIR/ldd_xr_steamvr_spatial_scene.txt" || true

echo "[install_xr_steamvr_spatial_scene] installed: $INSTALL_BIN_DIR/xr_steamvr_spatial_scene"
echo "[install_xr_steamvr_spatial_scene] run: $INSTALL_BIN_DIR/scripts/linux/start_xr_steamvr_spatial_scene.sh"
