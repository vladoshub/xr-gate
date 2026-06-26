#!/usr/bin/env bash
set -euo pipefail

log() { echo "[install_steamvr_video_overlay] $*" >&2; }
fatal() { echo "[ERROR] $*" >&2; exit 1; }

expand_tilde() {
  local value="$1"
  case "$value" in
    "~") printf '%s\n' "$HOME" ;;
    "~/"*) printf '%s\n' "$HOME/${value#"~/"}" ;;
    *) printf '%s\n' "$value" ;;
  esac
}

ROOT_PROJECT="${ROOT_PROJECT:-$HOME/src/xr_tracking}"
ROOT_PROJECT="$(expand_tilde "$ROOT_PROJECT")"

APP_DIR="${APP_DIR:-$ROOT_PROJECT/apps/steamvr/video_overlay}"
APP_DIR="$(expand_tilde "$APP_DIR")"

BUILD_TYPE="${BUILD_TYPE:-RelWithDebInfo}"
BUILD_NAME="${BUILD_NAME:-relwithdebinfo}"
BUILD_DIR="${BUILD_DIR:-$ROOT_PROJECT/build/apps/steamvr/video_overlay/$BUILD_NAME}"
BUILD_DIR="$(expand_tilde "$BUILD_DIR")"

INSTALL_BIN_DIR="${INSTALL_BIN_DIR:-$ROOT_PROJECT/bin/apps/steamvr/video_overlay}"
INSTALL_BIN_DIR="$(expand_tilde "$INSTALL_BIN_DIR")"

CLEAN_BUILD="${CLEAN_BUILD:-0}"
INSTALL_APT_DEPS="${INSTALL_APT_DEPS:-1}"
BUILD_JOBS="${BUILD_JOBS:-$(nproc)}"

XR_OPENVR_SDK_ROOT="${XR_OPENVR_SDK_ROOT:-${OPENVR_SDK_ROOT:-}}"
if [[ -n "$XR_OPENVR_SDK_ROOT" ]]; then
  XR_OPENVR_SDK_ROOT="$(expand_tilde "$XR_OPENVR_SDK_ROOT")"
elif [[ -f "$ROOT_PROJECT/third_party/openvr/headers/openvr.h" ]]; then
  XR_OPENVR_SDK_ROOT="$ROOT_PROJECT/third_party/openvr"
elif [[ -f "$ROOT_PROJECT/third-party/openvr/headers/openvr.h" ]]; then
  XR_OPENVR_SDK_ROOT="$ROOT_PROJECT/third-party/openvr"
fi

[[ -d "$ROOT_PROJECT" ]] || fatal "ROOT_PROJECT not found: $ROOT_PROJECT"
[[ -f "$APP_DIR/src/main.cpp" ]] || fatal "main.cpp not found: $APP_DIR/src/main.cpp"
[[ -f "$APP_DIR/CMakeLists.txt" ]] || fatal "CMakeLists.txt not found: $APP_DIR/CMakeLists.txt"
[[ -d "$ROOT_PROJECT/shared/include/xr_video" ]] || fatal "shared xr_video headers not found: $ROOT_PROJECT/shared/include/xr_video"
[[ -n "$XR_OPENVR_SDK_ROOT" ]] || fatal "OpenVR SDK not found. Set XR_OPENVR_SDK_ROOT or OPENVR_SDK_ROOT."
[[ -f "$XR_OPENVR_SDK_ROOT/headers/openvr.h" ]] || fatal "openvr.h not found under $XR_OPENVR_SDK_ROOT/headers"

command -v cmake >/dev/null 2>&1 || fatal "cmake not found"
command -v c++ >/dev/null 2>&1 || fatal "C++ compiler not found"

if [[ "$INSTALL_APT_DEPS" == "1" ]]; then
  log "Installing basic build dependencies"
  sudo apt update
  sudo apt install -y cmake build-essential pkg-config nlohmann-json3-dev
fi

CMAKE_PREFIX_PATH_EXTRA="${CMAKE_PREFIX_PATH:-}"
for prefix in \
  "$ROOT_PROJECT/build/backends/basalt_vio/relwithdebinfo/vcpkg_installed/x64-linux" \
  "$ROOT_PROJECT/third_party/basalt/build/relwithdebinfo/vcpkg_installed/x64-linux" \
  "$ROOT_PROJECT/third-party/basalt/build/relwithdebinfo/vcpkg_installed/x64-linux"; do
  if [[ -d "$prefix" ]]; then
    if [[ -n "$CMAKE_PREFIX_PATH_EXTRA" ]]; then
      CMAKE_PREFIX_PATH_EXTRA="$prefix;$CMAKE_PREFIX_PATH_EXTRA"
    else
      CMAKE_PREFIX_PATH_EXTRA="$prefix"
    fi
  fi
done

if [[ "$CLEAN_BUILD" == "1" ]]; then
  log "Removing build dir: $BUILD_DIR"
  rm -rf "$BUILD_DIR"
fi

mkdir -p "$BUILD_DIR" "$INSTALL_BIN_DIR"

log "ROOT_PROJECT=$ROOT_PROJECT"
log "APP_DIR=$APP_DIR"
log "XR_OPENVR_SDK_ROOT=$XR_OPENVR_SDK_ROOT"
log "BUILD_DIR=$BUILD_DIR"
log "INSTALL_BIN_DIR=$INSTALL_BIN_DIR"
log "CMAKE_PREFIX_PATH=$CMAKE_PREFIX_PATH_EXTRA"

cmake -S "$APP_DIR" -B "$BUILD_DIR" \
  -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
  -DXR_TRACKING_ROOT="$ROOT_PROJECT" \
  -DXR_SHARED_INCLUDE_DIR="$ROOT_PROJECT/shared/include" \
  -DXR_OPENVR_SDK_ROOT="$XR_OPENVR_SDK_ROOT" \
  -DCMAKE_PREFIX_PATH="$CMAKE_PREFIX_PATH_EXTRA" \
  ${NLOHMANN_JSON_INCLUDE_DIR:+-DNLOHMANN_JSON_INCLUDE_DIR="$NLOHMANN_JSON_INCLUDE_DIR"}

cmake --build "$BUILD_DIR" --target steamvr_video_overlay -j"$BUILD_JOBS"

[[ -x "$BUILD_DIR/steamvr_video_overlay" ]] || fatal "Built binary not found: $BUILD_DIR/steamvr_video_overlay"
cp "$BUILD_DIR/steamvr_video_overlay" "$INSTALL_BIN_DIR/steamvr_video_overlay"
chmod +x "$INSTALL_BIN_DIR/steamvr_video_overlay"

# Keep runtime simple: put openvr_api next to the binary, same pattern as the driver package.
OPENVR_LIB=""
for candidate in \
  "$XR_OPENVR_SDK_ROOT/bin/linux64/libopenvr_api.so" \
  "$XR_OPENVR_SDK_ROOT/lib/linux64/libopenvr_api.so" \
  "$XR_OPENVR_SDK_ROOT/bin/linux64/openvr_api.so" \
  "$XR_OPENVR_SDK_ROOT/lib/linux64/openvr_api.so"; do
  if [[ -f "$candidate" ]]; then
    OPENVR_LIB="$candidate"
    break
  fi
done

if [[ -n "$OPENVR_LIB" ]]; then
  cp "$OPENVR_LIB" "$INSTALL_BIN_DIR/"
fi

rm -rf "$INSTALL_BIN_DIR/scripts"
cp -a "$APP_DIR/scripts" "$INSTALL_BIN_DIR/scripts"
cp "$APP_DIR/README.md" "$INSTALL_BIN_DIR/README.md"

cat <<EOF2

[OK] steamvr_video_overlay built:
  $INSTALL_BIN_DIR/steamvr_video_overlay

Quick check:
  $INSTALL_BIN_DIR/steamvr_video_overlay --help

Typical launch:
  $APP_DIR/scripts/linux/start_steamvr_video_overlay.sh
EOF2
