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
  echo "[install_bridges] $*" >&2
}

ROOT_PROJECT="${ROOT_PROJECT:-$HOME/src/xr_tracking}"
ROOT_PROJECT="$(expand_tilde "$ROOT_PROJECT")"

BRIDGES_DIR="${BRIDGES_DIR:-$ROOT_PROJECT/bridges}"
BRIDGES_DIR="$(expand_tilde "$BRIDGES_DIR")"

CMAKE_BUILD_TYPE="${CMAKE_BUILD_TYPE:-RelWithDebInfo}"
CMAKE_BUILD_DIR_NAME="${CMAKE_BUILD_DIR_NAME:-relwithdebinfo}"
BUILD_DIR="${BUILD_DIR:-$ROOT_PROJECT/build/bridges/$CMAKE_BUILD_DIR_NAME}"
BUILD_DIR="$(expand_tilde "$BUILD_DIR")"

INSTALL_BIN_DIR="${INSTALL_BIN_DIR:-$ROOT_PROJECT/bin/bridges}"
INSTALL_BIN_DIR="$(expand_tilde "$INSTALL_BIN_DIR")"

VCPKG_TRIPLET="${VCPKG_TRIPLET:-x64-linux}"
INSTALL_APT_DEPS="${INSTALL_APT_DEPS:-1}"
BUILD_JOBS="${BUILD_JOBS:-$(nproc)}"

find_optional_vcpkg_installed() {
  local candidates=()

  if [[ -n "${BASALT_VCPKG_INSTALLED:-}" ]]; then
    BASALT_VCPKG_INSTALLED="$(expand_tilde "$BASALT_VCPKG_INSTALLED")"
    candidates+=("$BASALT_VCPKG_INSTALLED")
  fi

  candidates+=(
    "$ROOT_PROJECT/build/backends/basalt_vio/relwithdebinfo/vcpkg_installed/$VCPKG_TRIPLET"
    "$ROOT_PROJECT/build/backends/basalt_vio/debug/vcpkg_installed/$VCPKG_TRIPLET"
    "$ROOT_PROJECT/third_party/basalt/build/relwithdebinfo/vcpkg_installed/$VCPKG_TRIPLET"
    "$ROOT_PROJECT/third_party/basalt/build/debug/vcpkg_installed/$VCPKG_TRIPLET"
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

if [[ ! -d "$BRIDGES_DIR" ]]; then
  echo "[ERROR] bridges source dir not found: $BRIDGES_DIR" >&2
  exit 1
fi

if [[ ! -f "$BRIDGES_DIR/CMakeLists.txt" ]]; then
  echo "[ERROR] CMakeLists.txt not found in bridges dir: $BRIDGES_DIR" >&2
  exit 1
fi

if [[ ! -d "$ROOT_PROJECT/shared/include" ]]; then
  echo "[ERROR] shared/include not found: $ROOT_PROJECT/shared/include" >&2
  exit 1
fi

if [[ "$INSTALL_APT_DEPS" == "1" ]]; then
  sudo apt update
  sudo apt install -y cmake build-essential pkg-config libcli11-dev nlohmann-json3-dev
fi

CMAKE_PREFIX_PATH_ENTRIES=()

add_prefix_if_exists() {
  local p="$1"
  if [[ -n "$p" && -d "$p" ]]; then
    CMAKE_PREFIX_PATH_ENTRIES+=("$p")
  fi
}

# Optional explicit dependency prefix from caller.
if [[ -n "${BRIDGES_DEPS_PREFIX:-}" ]]; then
  BRIDGES_DEPS_PREFIX="$(expand_tilde "$BRIDGES_DEPS_PREFIX")"
  add_prefix_if_exists "$BRIDGES_DEPS_PREFIX"
fi

# Optional fallback: reuse Basalt/vcpkg dependencies only when that tree already exists.
OPTIONAL_VCPKG_INSTALLED=""
if OPTIONAL_VCPKG_INSTALLED="$(find_optional_vcpkg_installed 2>/dev/null)"; then
  add_prefix_if_exists "$OPTIONAL_VCPKG_INSTALLED"
fi

log "ROOT_PROJECT=$ROOT_PROJECT"
log "BRIDGES_DIR=$BRIDGES_DIR"
log "BUILD_DIR=$BUILD_DIR"
log "INSTALL_BIN_DIR=$INSTALL_BIN_DIR"
if [[ -n "$OPTIONAL_VCPKG_INSTALLED" ]]; then
  log "OPTIONAL_VCPKG_INSTALLED=$OPTIONAL_VCPKG_INSTALLED"
else
  log "OPTIONAL_VCPKG_INSTALLED=<none>; relying on system CMake packages"
fi

mkdir -p "$BUILD_DIR" "$INSTALL_BIN_DIR"

CMAKE_ARGS=(
  -S "$BRIDGES_DIR"
  -B "$BUILD_DIR"
  -DCMAKE_BUILD_TYPE="$CMAKE_BUILD_TYPE"
  -DXR_TRACKING_ROOT="$ROOT_PROJECT"
)

if [[ -n "$OPTIONAL_VCPKG_INSTALLED" ]]; then
  CMAKE_ARGS+=("-DVCPKG_INSTALLED=$OPTIONAL_VCPKG_INSTALLED")
fi

if (( ${#CMAKE_PREFIX_PATH_ENTRIES[@]} > 0 )); then
  CMAKE_PREFIX_JOINED="$(IFS=';'; echo "${CMAKE_PREFIX_PATH_ENTRIES[*]}")"
  CMAKE_ARGS+=("-DCMAKE_PREFIX_PATH=$CMAKE_PREFIX_JOINED")
fi

cmake "${CMAKE_ARGS[@]}"

cmake --build "$BUILD_DIR" -j"$BUILD_JOBS" \
  --target capture_net_bridge \
  --target tracking_udp_bridge \
  --target tracking_udp_debug_receiver \
  --target spatial_proxy_mesh_udp_debug_receiver

for exe in capture_net_bridge tracking_udp_bridge tracking_udp_debug_receiver spatial_proxy_mesh_udp_debug_receiver; do
  if [[ ! -x "$BUILD_DIR/$exe" ]]; then
    echo "[ERROR] Built executable not found: $BUILD_DIR/$exe" >&2
    exit 1
  fi
  cp "$BUILD_DIR/$exe" "$INSTALL_BIN_DIR/$exe"
  chmod +x "$INSTALL_BIN_DIR/$exe"
done

log "Installed bridge binaries:"
ls -lh "$INSTALL_BIN_DIR" >&2
