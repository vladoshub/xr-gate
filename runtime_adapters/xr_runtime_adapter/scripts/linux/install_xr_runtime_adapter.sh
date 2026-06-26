#!/usr/bin/env bash
set -euo pipefail

log() { echo "[install_xr_runtime_adapter] $*" >&2; }
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

ADAPTER_DIR="${ADAPTER_DIR:-$ROOT_PROJECT/runtime_adapters/xr_runtime_adapter}"
ADAPTER_DIR="$(expand_tilde "$ADAPTER_DIR")"

BUILD_TYPE="${BUILD_TYPE:-RelWithDebInfo}"
BUILD_NAME="${BUILD_NAME:-relwithdebinfo}"
BUILD_DIR="${BUILD_DIR:-$ROOT_PROJECT/build/runtime_adapters/xr_runtime_adapter/$BUILD_NAME}"
BUILD_DIR="$(expand_tilde "$BUILD_DIR")"

INSTALL_BIN_DIR="${INSTALL_BIN_DIR:-$ROOT_PROJECT/bin/runtime_adapters/xr_runtime_adapter}"
INSTALL_BIN_DIR="$(expand_tilde "$INSTALL_BIN_DIR")"

CLEAN_BUILD="${CLEAN_BUILD:-0}"
BUILD_JOBS="${BUILD_JOBS:-$(nproc 2>/dev/null || echo 2)}"

[[ -d "$ROOT_PROJECT" ]] || fatal "ROOT_PROJECT not found: $ROOT_PROJECT"
[[ -f "$ADAPTER_DIR/src/xr_runtime_adapter.cpp" ]] || fatal "xr_runtime_adapter.cpp not found: $ADAPTER_DIR/src/xr_runtime_adapter.cpp"
[[ -f "$ADAPTER_DIR/CMakeLists.txt" ]] || fatal "CMakeLists.txt not found: $ADAPTER_DIR/CMakeLists.txt"
[[ -d "$ROOT_PROJECT/shared/include" ]] || fatal "shared/include not found: $ROOT_PROJECT/shared/include"

command -v cmake >/dev/null 2>&1 || fatal "cmake not found"
command -v c++ >/dev/null 2>&1 || fatal "C++ compiler not found"

CMAKE_PREFIX_PATH_EXTRA="${CMAKE_PREFIX_PATH:-}"
for prefix in \
  "$ROOT_PROJECT/build/backends/basalt_vio/relwithdebinfo/vcpkg_installed/x64-linux" \
  "$ROOT_PROJECT/third_party/basalt/build/relwithdebinfo/vcpkg_installed/x64-linux"; do
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
log "ADAPTER_DIR=$ADAPTER_DIR"
log "BUILD_DIR=$BUILD_DIR"
log "INSTALL_BIN_DIR=$INSTALL_BIN_DIR"
log "CMAKE_PREFIX_PATH=$CMAKE_PREFIX_PATH_EXTRA"

cmake -S "$ADAPTER_DIR" -B "$BUILD_DIR" \
  -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
  -DXR_TRACKING_ROOT="$ROOT_PROJECT" \
  -DXR_SHARED_INCLUDE_DIR="$ROOT_PROJECT/shared/include" \
  -DCMAKE_PREFIX_PATH="$CMAKE_PREFIX_PATH_EXTRA" \
  ${CLI11_INCLUDE_DIR:+-DCLI11_INCLUDE_DIR="$CLI11_INCLUDE_DIR"} \
  ${NLOHMANN_JSON_INCLUDE_DIR:+-DNLOHMANN_JSON_INCLUDE_DIR="$NLOHMANN_JSON_INCLUDE_DIR"}

target_help="$(cmake --build "$BUILD_DIR" --target help 2>&1 || true)"
if ! grep -qE "(^|[[:space:]])xr_runtime_adapter([[:space:]]|$)" <<<"$target_help"; then
  help_targets="$(printf '%s\n' "$target_help" | awk 'NR <= 80 { print }' | paste -sd ';' -)"
  fatal "CMake target xr_runtime_adapter was not generated. Check $ADAPTER_DIR/CMakeLists.txt; generated targets are: $help_targets"
fi

cmake --build "$BUILD_DIR" --target xr_runtime_adapter -j"$BUILD_JOBS"

cp "$BUILD_DIR/xr_runtime_adapter" "$INSTALL_BIN_DIR/xr_runtime_adapter"
chmod +x "$INSTALL_BIN_DIR/xr_runtime_adapter"

cat <<EOF2

[OK] xr_runtime_adapter built:
  $INSTALL_BIN_DIR/xr_runtime_adapter

Quick check:
  $INSTALL_BIN_DIR/xr_runtime_adapter --help

Typical SHM runtime launch:
  $ADAPTER_DIR/scripts/linux/start_xr_runtime_adapter_shm.sh
EOF2
