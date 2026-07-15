#!/usr/bin/env bash
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=/dev/null
source "$SCRIPT_DIR/../lib/device_runtime.sh"
xr_load_device_env
xr_exec_runtime_script mercury_hand_tracking \
  scripts/backends/mercury_hand_tracking/start_hand_tracking.sh \
  backends/mercury_hand_tracking/scripts/linux/start_hand_tracking.sh \
  "$@"
