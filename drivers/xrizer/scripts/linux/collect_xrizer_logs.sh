#!/usr/bin/env bash
set -euo pipefail

OUT_DIR="${1:-/tmp/xrizer_logs_$(date +%Y%m%d_%H%M%S)}"
mkdir -p "$OUT_DIR"

copy_if_exists() {
  local src="$1"
  local dst="$2"
  if [[ -e "$src" ]]; then
    mkdir -p "$(dirname "$dst")"
    cp -a "$src" "$dst"
  fi
}

STATE_HOME="${XDG_STATE_HOME:-$HOME/.local/state}"
CONFIG_HOME="${XDG_CONFIG_HOME:-$HOME/.config}"

copy_if_exists "$STATE_HOME/xrizer/xrizer.txt" "$OUT_DIR/xrizer.txt"
copy_if_exists "$CONFIG_HOME/openvr/openvrpaths.vrpath" "$OUT_DIR/openvrpaths.vrpath"
copy_if_exists "$HOME/.config/openvr/logs/vrserver.txt" "$OUT_DIR/openvr_vrserver.txt"
copy_if_exists "$HOME/.config/openvr/logs/vrcompositor.txt" "$OUT_DIR/openvr_vrcompositor.txt"

if [[ -n "${XR_RUNTIME_JSON:-}" ]]; then
  printf '%s\n' "$XR_RUNTIME_JSON" > "$OUT_DIR/XR_RUNTIME_JSON.txt"
fi
if [[ -n "${VR_OVERRIDE:-}" ]]; then
  printf '%s\n' "$VR_OVERRIDE" > "$OUT_DIR/VR_OVERRIDE.txt"
fi
if command -v ps >/dev/null 2>&1; then
  ps -eo pid,ppid,stat,cmd | grep -Ei 'monado|xrizer|steam|vrserver|vrcompositor|openvr' | grep -v grep > "$OUT_DIR/processes.txt" 2>&1 || true
fi

tar_path="$OUT_DIR.tar.gz"
tar -C "$(dirname "$OUT_DIR")" -czf "$tar_path" "$(basename "$OUT_DIR")"
echo "$tar_path"
