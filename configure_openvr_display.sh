#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SCRIPT="$ROOT/bin/scripts/drivers/steam_vr/configure_openvr_display.py"
if [[ ! -f "$SCRIPT" ]]; then
  SCRIPT="$ROOT/drivers/steam_vr/scripts/linux/configure_openvr_display.py"
fi
exec python3 "$SCRIPT" "$@"
