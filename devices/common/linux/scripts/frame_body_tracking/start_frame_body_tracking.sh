#!/usr/bin/env bash
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=/dev/null
source "$SCRIPT_DIR/../lib/device_runtime.sh"
xr_load_device_env
xr_exec_runtime_script frame_body_tracking \
  scripts/backends/frame_body_tracking/start_frame_body_tracking.sh \
  backends/frame_body_tracking/scripts/linux/start_frame_body_tracking.sh \
  "$@"
