#!/usr/bin/env bash
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=/dev/null
source "$SCRIPT_DIR/../lib/device_runtime.sh"
xr_load_device_env_or_common
[[ -x "$STEAMVR_VIDEO_OVERLAY_START_SCRIPT" ]] || xr_common_fatal "SteamVR video overlay start script not found: $STEAMVR_VIDEO_OVERLAY_START_SCRIPT"
export APP_BIN="${APP_BIN:-$STEAMVR_VIDEO_OVERLAY_BIN}"
export BIN_DIR="${BIN_DIR:-$STEAMVR_VIDEO_OVERLAY_DIR}"
exec "$STEAMVR_VIDEO_OVERLAY_START_SCRIPT" "$@"
