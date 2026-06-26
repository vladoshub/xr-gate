#!/usr/bin/env bash
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ENV_FILE="${XR_DEVICE_ENV:-$(cd "$SCRIPT_DIR/../../.." && pwd)/xreal_ultra.env}"
# shellcheck source=/dev/null
source "$ENV_FILE"

if [[ ! -x "$XR_STEAMVR_SPATIAL_SCENE_START_SCRIPT" ]]; then
  echo "[device/start_xr_steamvr_spatial_scene][ERROR] start script not found: $XR_STEAMVR_SPATIAL_SCENE_START_SCRIPT" >&2
  echo "[device/start_xr_steamvr_spatial_scene][ERROR] Build package with XR_BUILD_ONLY=steamvr_spatial_scene or full install_xreal_ultra_out.sh" >&2
  exit 2
fi

export ROOT_PROJECT="$XR_ROOT_PROJECT"
export APP_BIN="$XR_STEAMVR_SPATIAL_SCENE_BIN"
export SPATIAL_SCENE_CONFIG
exec "$XR_STEAMVR_SPATIAL_SCENE_START_SCRIPT" "$@"
