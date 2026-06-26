#!/usr/bin/env bash
set -euo pipefail

ROOT_PROJECT="${ROOT_PROJECT:-$HOME/src/xr_tracking}"
VIEWER_DIR="$ROOT_PROJECT/tools/runtime_debug_viewer"
CONFIG_PATH="${CONFIG_PATH:-$VIEWER_DIR/configs/xr_runtime_debug_viewer.yaml}"

exec python3 "$VIEWER_DIR/xr_runtime_debug_viewer.py" --config "$CONFIG_PATH" "$@"
