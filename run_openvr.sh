#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export XR_OPENVR_REGISTER_SCRIPT="${XR_OPENVR_REGISTER_SCRIPT:-$ROOT/drivers/openvr_driver/scripts/register_driver.sh}"
exec python3 "$ROOT/drivers/steam_vr/scripts/linux/start_openvr.py" "$@"
