#!/usr/bin/env bash
set -euo pipefail

log() { echo "[install_override_controller] $*" >&2; }
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

TARGET_USER="${SUDO_USER:-${USER:-}}"

BACKEND_DIR="${BACKEND_DIR:-$ROOT_PROJECT/override_controller}"
BACKEND_DIR="$(expand_tilde "$BACKEND_DIR")"

BUILD_TYPE="${BUILD_TYPE:-RelWithDebInfo}"
BUILD_NAME="${BUILD_NAME:-relwithdebinfo}"
BUILD_DIR="${BUILD_DIR:-$ROOT_PROJECT/build/override_controller/$BUILD_NAME}"
BUILD_DIR="$(expand_tilde "$BUILD_DIR")"

INSTALL_BIN_DIR="${INSTALL_BIN_DIR:-$ROOT_PROJECT/bin/override_controller}"
INSTALL_BIN_DIR="$(expand_tilde "$INSTALL_BIN_DIR")"

CLEAN_BUILD="${CLEAN_BUILD:-0}"
BUILD_JOBS="${BUILD_JOBS:-$(nproc 2>/dev/null || echo 2)}"

[[ -d "$ROOT_PROJECT" ]] || fatal "ROOT_PROJECT not found: $ROOT_PROJECT"
[[ -f "$BACKEND_DIR/src/main.cpp" ]] || fatal "override_controller source not found: $BACKEND_DIR/src/main.cpp"
[[ -f "$BACKEND_DIR/CMakeLists.txt" ]] || fatal "CMakeLists.txt not found: $BACKEND_DIR/CMakeLists.txt"
[[ -d "$ROOT_PROJECT/shared/include" ]] || fatal "shared/include not found: $ROOT_PROJECT/shared/include"

command -v cmake >/dev/null 2>&1 || fatal "cmake not found"
command -v c++ >/dev/null 2>&1 || fatal "C++ compiler not found"

if [[ "${ADD_INPUT_GROUP:-0}" == "1" ]]; then
  if [[ -z "$TARGET_USER" || "$TARGET_USER" == "root" ]]; then
    echo "[install_override_controller][ERROR] Cannot determine target desktop user for input group."
    echo "Run as normal user, or set TARGET_USER explicitly."
    exit 1
  fi

  if id -nG "$TARGET_USER" | tr ' ' '\n' | grep -qx "input"; then
    echo "[install_override_controller] User '$TARGET_USER' is already in input group."
  else
    echo "[install_override_controller] Adding '$TARGET_USER' to input group..."
    sudo usermod -aG input "$TARGET_USER"
    cat <<EOF

[install_override_controller] Added '$TARGET_USER' to input group.

IMPORTANT:
  This does not affect the current shell.
  Log out/in, reboot, or run:

    newgrp input

Then verify:

    groups | tr ' ' '\\n' | grep -x input

EOF
  fi
else
  cat <<EOF

[install_override_controller] override_controller needs read access to /dev/input/event*.

Recommended one-time setup:

  sudo usermod -aG input "$TARGET_USER"

Then log out/in or run:

  newgrp input

For temporary testing:

  sudo setfacl -m u:$TARGET_USER:rw /dev/input/event*

Or run start script with:

  USE_SUDO=1 override_controller/scripts/linux/start_override_controller.sh

To let this installer add the group automatically, run:

  ADD_INPUT_GROUP=1 override_controller/scripts/linux/install_override_controller.sh

EOF
fi

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
log "BACKEND_DIR=$BACKEND_DIR"
log "BUILD_DIR=$BUILD_DIR"
log "INSTALL_BIN_DIR=$INSTALL_BIN_DIR"
log "CMAKE_PREFIX_PATH=$CMAKE_PREFIX_PATH_EXTRA"

cmake -S "$BACKEND_DIR" -B "$BUILD_DIR" \
  -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
  -DXR_TRACKING_ROOT="$ROOT_PROJECT" \
  -DXR_SHARED_INCLUDE_DIR="$ROOT_PROJECT/shared/include" \
  -DCMAKE_PREFIX_PATH="$CMAKE_PREFIX_PATH_EXTRA" \
  ${NLOHMANN_JSON_INCLUDE_DIR:+-DNLOHMANN_JSON_INCLUDE_DIR="$NLOHMANN_JSON_INCLUDE_DIR"}

cmake --build "$BUILD_DIR" --target override_controller -j"$BUILD_JOBS"

cp "$BUILD_DIR/override_controller" "$INSTALL_BIN_DIR/override_controller"
chmod +x "$INSTALL_BIN_DIR/override_controller"


cat <<EOF2

[OK] override_controller built:
  $INSTALL_BIN_DIR/override_controller

Quick checks:
  $INSTALL_BIN_DIR/override_controller --help
  $INSTALL_BIN_DIR/override_controller --list-devices

Typical launch via start script:
  $ROOT_PROJECT/override_controller/scripts/linux/start_override_controller.sh

Direct launch:
  $INSTALL_BIN_DIR/override_controller
EOF2
