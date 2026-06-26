#!/bin/bash
set -euo pipefail

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
INSTALL_BIN_DIR="${INSTALL_BIN_DIR:-$ROOT_PROJECT/bin/xreal_display_helper}"
INSTALL_BIN_DIR="$(expand_tilde "$INSTALL_BIN_DIR")"
BUILD_DIR="${BUILD_DIR:-$ROOT_PROJECT/build/tools/xreal_ultra/xreal_display_helper}"
BUILD_DIR="$(expand_tilde "$BUILD_DIR")"
BUILD_JOBS="${BUILD_JOBS:-$(nproc 2>/dev/null || echo 2)}"

cd "$ROOT_PROJECT"

sudo apt update
sudo apt install -y cmake build-essential pkg-config libhidapi-dev

cmake -S tools/xreal_ultra/xreal_display_helper \
  -B "$BUILD_DIR" \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo

cmake --build "$BUILD_DIR" -j"$BUILD_JOBS"

mkdir -p "$INSTALL_BIN_DIR"

cp "$BUILD_DIR/xreal_display_helper" \
   "$INSTALL_BIN_DIR/xreal_display_helper"

chmod +x "$INSTALL_BIN_DIR/xreal_display_helper"

echo "[install_xreal_helper] installed: $INSTALL_BIN_DIR/xreal_display_helper" >&2
