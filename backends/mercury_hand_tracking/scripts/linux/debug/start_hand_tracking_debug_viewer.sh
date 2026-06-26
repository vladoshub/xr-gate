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

ROOT_PROJECT="${ROOT_PROJECT:-$HOME/src/xr_tracking}"
ROOT_PROJECT="$(expand_tilde "$ROOT_PROJECT")"
VIEWER_SCRIPT="${VIEWER_SCRIPT:-$ROOT_PROJECT/backends/mercury_hand_tracking/tools/debug/mercury_xr_raw2d_overlay_viewer.py}"
VIEWER_SCRIPT="$(expand_tilde "$VIEWER_SCRIPT")"
DUMP_DIR="${DUMP_DIR:-/tmp/xr_mercury_debug}"
SCALE="${SCALE:-1.0}"

python3 "$VIEWER_SCRIPT" \
  --dump-dir "$DUMP_DIR" \
  --wait \
  --scale "$SCALE"
