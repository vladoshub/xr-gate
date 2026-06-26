#!/usr/bin/env bash
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ENV_FILE="${XR_DEVICE_ENV:-$(cd "$SCRIPT_DIR/../../.." && pwd)/xreal_ultra.env}"
# shellcheck source=/dev/null
source "$ENV_FILE"

if [[ ! -x "$STEAMVR_VIDEO_OVERLAY_START_SCRIPT" ]]; then
  echo "[device/start_steamvr_video_overlay][ERROR] start script not found: $STEAMVR_VIDEO_OVERLAY_START_SCRIPT" >&2
  echo "[device/start_steamvr_video_overlay][ERROR] Build package with XR_BUILD_ONLY=steamvr_video_overlay or full install_xreal_ultra_out.sh" >&2
  exit 2
fi

export ROOT_PROJECT="$XR_ROOT_PROJECT"
export BIN_DIR="$STEAMVR_VIDEO_OVERLAY_DIR"
export STEAMVR_VIDEO_OVERLAY_BIN
exec "$STEAMVR_VIDEO_OVERLAY_START_SCRIPT" "$@"
