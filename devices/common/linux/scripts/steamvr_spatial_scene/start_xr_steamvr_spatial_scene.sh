#!/usr/bin/env bash
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=/dev/null
source "$SCRIPT_DIR/../lib/device_runtime.sh"
xr_load_device_env_or_common
[[ -x "$XR_STEAMVR_SPATIAL_SCENE_START_SCRIPT" ]] || xr_common_fatal "SteamVR spatial scene start script not found: $XR_STEAMVR_SPATIAL_SCENE_START_SCRIPT"
export APP_BIN="${APP_BIN:-$XR_STEAMVR_SPATIAL_SCENE_BIN}"
export SPATIAL_SCENE_CONFIG
exec "$XR_STEAMVR_SPATIAL_SCENE_START_SCRIPT" "$@"
