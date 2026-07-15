#!/usr/bin/env bash
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=/dev/null
source "$SCRIPT_DIR/../lib/device_runtime.sh"
xr_load_device_env
export BIN_DIR="${BIN_DIR:-$BIN_DIR_XR_VIDEO}"
xr_exec_runtime_script xr_video \
  scripts/backends/xr_video/start_xr_video_backend.sh \
  backends/xr_video/scripts/linux/start_xr_video_backend.sh \
  "$@"
