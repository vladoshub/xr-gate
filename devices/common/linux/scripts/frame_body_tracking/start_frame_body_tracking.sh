#!/usr/bin/env bash
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=/dev/null
source "$SCRIPT_DIR/../lib/device_runtime.sh"
xr_load_device_env
# xr_client loads the tracking layer before spawning backends. Also support a
# direct launch with the same tracking layer or its device-local default.
if [[ -z "${XR_TRACKING_HOME:-}" ]]; then
  tracking_env="${XR_TRACKING_ENV:-$XR_DEVICE_HOME/tracking.env}"
  if [[ -f "$tracking_env" ]]; then
    source "$tracking_env"
  elif [[ -n "${XR_TRACKING_ENV:-}" ]]; then
    xr_common_fatal "tracking env not found: $tracking_env"
  fi
fi

frame_profile="${XR_TRACKING_HOME:-$XR_DEVICE_HOME}/frame_body_tracking.env"
if [[ -f "$frame_profile" ]]; then
  source "$frame_profile"
fi
xr_exec_runtime_script frame_body_tracking \
  scripts/backends/frame_body_tracking/start_frame_body_tracking.sh \
  backends/frame_body_tracking/scripts/linux/start_frame_body_tracking.sh \
  "$@"
