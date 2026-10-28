#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SCRIPT="$ROOT/bin/scripts/drivers/steam_vr/start_openvr.py"
if [[ ! -f "$SCRIPT" ]]; then
  SCRIPT="$ROOT/drivers/steam_vr/scripts/linux/start_openvr.py"
  export XR_OPENVR_REGISTER_SCRIPT="${XR_OPENVR_REGISTER_SCRIPT:-$ROOT/drivers/openvr_driver/scripts/register_driver.sh}"
fi
exec python3 "$SCRIPT" "$@"
