#!/usr/bin/env bash
set -euo pipefail

expand_tilde() {
  local value="$1"
  case "$value" in
    "~") printf '%s\n' "$HOME" ;;
    "~/"*) printf '%s\n' "$HOME/${value#"~/"}" ;;
    *) printf '%s\n' "$value" ;;
  esac
}

log() {
  echo "[install_xr_video] $*" >&2
}

ROOT_PROJECT="${ROOT_PROJECT:-$HOME/src/xr_tracking}"
BACKEND_DIR="${BACKEND_DIR:-$ROOT_PROJECT/backends/xr_video}"
BASALT_DIR="${BASALT_DIR:-$ROOT_PROJECT/third_party/basalt}"
VCPKG_TRIPLET="${VCPKG_TRIPLET:-x64-linux}"

CMAKE_BUILD_TYPE="${CMAKE_BUILD_TYPE:-RelWithDebInfo}"
BUILD_NAME="${BUILD_NAME:-relwithdebinfo}"
BUILD_ROOT="${BUILD_ROOT:-$ROOT_PROJECT/build/backends/xr_video}"
BUILD_DIR="${BUILD_DIR:-$BUILD_ROOT/$BUILD_NAME}"
INSTALL_BIN_DIR="${INSTALL_BIN_DIR:-$ROOT_PROJECT/bin/backends/xr_video}"

INSTALL_APT_DEPS="${INSTALL_APT_DEPS:-1}"
BUILD_JOBS="${BUILD_JOBS:-$(nproc)}"
BUILD_TARGETS="${BUILD_TARGETS:-xr_video_backend xr_video_monitor}"

ROOT_PROJECT="$(expand_tilde "$ROOT_PROJECT")"
BACKEND_DIR="$(expand_tilde "$BACKEND_DIR")"
BASALT_DIR="$(expand_tilde "$BASALT_DIR")"
BUILD_ROOT="$(expand_tilde "$BUILD_ROOT")"
BUILD_DIR="$(expand_tilde "$BUILD_DIR")"
INSTALL_BIN_DIR="$(expand_tilde "$INSTALL_BIN_DIR")"

if [[ -n "${BASALT_VCPKG_INSTALLED:-}" ]]; then
  BASALT_VCPKG_INSTALLED="$(expand_tilde "$BASALT_VCPKG_INSTALLED")"
fi

find_optional_vcpkg_installed() {
  local candidates=()

  if [[ -n "${BASALT_VCPKG_INSTALLED:-}" ]]; then
    candidates+=("$BASALT_VCPKG_INSTALLED")
  fi

  candidates+=(
    "$ROOT_PROJECT/build/backends/basalt_vio/relwithdebinfo/vcpkg_installed/$VCPKG_TRIPLET"
    "$ROOT_PROJECT/build/backends/basalt_vio/debug/vcpkg_installed/$VCPKG_TRIPLET"
    "$BASALT_DIR/build/relwithdebinfo/vcpkg_installed/$VCPKG_TRIPLET"
    "$BASALT_DIR/build/debug/vcpkg_installed/$VCPKG_TRIPLET"
  )

  local c
  for c in "${candidates[@]}"; do
    if [[ -d "$c" ]] && { [[ -f "$c/share/cli11/CLI11Config.cmake" ]] || [[ -f "$c/share/cli11/cli11-config.cmake" ]] || [[ -f "$c/include/CLI/CLI.hpp" ]]; }; then
      printf '%s\n' "$c"
      return 0
    fi
  done

  return 1
}

if [[ "$INSTALL_APT_DEPS" == "1" ]]; then
  log "Installing basic build dependencies"
  sudo apt update
  sudo apt install -y cmake build-essential pkg-config libcli11-dev nlohmann-json3-dev
fi

if [[ ! -d "$BACKEND_DIR" ]]; then
  echo "[ERROR] XR video backend directory not found: $BACKEND_DIR" >&2
  exit 1
fi

if [[ ! -f "$BACKEND_DIR/CMakeLists.txt" ]]; then
  echo "[ERROR] CMakeLists.txt not found in backend directory: $BACKEND_DIR" >&2
  exit 1
fi

if [[ ! -d "$ROOT_PROJECT/shared/include/capture_client" ]]; then
  echo "[ERROR] shared capture_client headers not found: $ROOT_PROJECT/shared/include/capture_client" >&2
  exit 1
fi

if [[ ! -d "$ROOT_PROJECT/shared/include/xr_video" ]]; then
  echo "[ERROR] shared xr_video headers not found: $ROOT_PROJECT/shared/include/xr_video" >&2
  exit 1
fi

CMAKE_PREFIX_PATH_ENTRIES=()

add_prefix_if_exists() {
  local p="$1"
  if [[ -n "$p" && -d "$p" ]]; then
    CMAKE_PREFIX_PATH_ENTRIES+=("$p")
  fi
}

# Optional explicit dependency prefix from caller.
if [[ -n "${XR_VIDEO_DEPS_PREFIX:-}" ]]; then
  XR_VIDEO_DEPS_PREFIX="$(expand_tilde "$XR_VIDEO_DEPS_PREFIX")"
  add_prefix_if_exists "$XR_VIDEO_DEPS_PREFIX"
fi

# Optional fallback: reuse Basalt/vcpkg dependencies only when that tree already exists.
OPTIONAL_VCPKG_INSTALLED=""
if OPTIONAL_VCPKG_INSTALLED="$(find_optional_vcpkg_installed 2>/dev/null)"; then
  add_prefix_if_exists "$OPTIONAL_VCPKG_INSTALLED"
fi

log "ROOT_PROJECT=$ROOT_PROJECT"
log "BACKEND_DIR=$BACKEND_DIR"
if [[ -n "$OPTIONAL_VCPKG_INSTALLED" ]]; then
  log "OPTIONAL_VCPKG_INSTALLED=$OPTIONAL_VCPKG_INSTALLED"
else
  log "OPTIONAL_VCPKG_INSTALLED=<none>; relying on system CMake packages"
fi
log "BUILD_DIR=$BUILD_DIR"
log "INSTALL_BIN_DIR=$INSTALL_BIN_DIR"

mkdir -p "$BUILD_DIR" "$INSTALL_BIN_DIR"

CMAKE_ARGS=(
  -S "$BACKEND_DIR"
  -B "$BUILD_DIR"
  -DCMAKE_BUILD_TYPE="$CMAKE_BUILD_TYPE"
  -DXR_TRACKING_ROOT="$ROOT_PROJECT"
  -DXR_SHARED_INCLUDE_DIR="$ROOT_PROJECT/shared/include"
)

if [[ -n "$OPTIONAL_VCPKG_INSTALLED" ]]; then
  CMAKE_ARGS+=("-DBASALT_VCPKG_INSTALLED=$OPTIONAL_VCPKG_INSTALLED")
fi

if (( ${#CMAKE_PREFIX_PATH_ENTRIES[@]} > 0 )); then
  CMAKE_PREFIX_JOINED="$(IFS=';'; echo "${CMAKE_PREFIX_PATH_ENTRIES[*]}")"
  CMAKE_ARGS+=("-DCMAKE_PREFIX_PATH=$CMAKE_PREFIX_JOINED")
fi

cmake "${CMAKE_ARGS[@]}"

# shellcheck disable=SC2086
cmake --build "$BUILD_DIR" -j"$BUILD_JOBS" --target $BUILD_TARGETS

for target in $BUILD_TARGETS; do
  if [[ ! -x "$BUILD_DIR/$target" ]]; then
    echo "[ERROR] Built executable not found: $BUILD_DIR/$target" >&2
    exit 1
  fi

  cp "$BUILD_DIR/$target" "$INSTALL_BIN_DIR/$target"
  chmod +x "$INSTALL_BIN_DIR/$target"
done

log "Installed binaries:"
ls -lh "$INSTALL_BIN_DIR" >&2
