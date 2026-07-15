#!/usr/bin/env bash
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=/dev/null
source "$SCRIPT_DIR/../lib/device_runtime.sh"
xr_load_device_env
xr_exec_runtime_script override_controller \
  scripts/override_controller/start_override_controller.sh \
  override_controller/scripts/linux/start_override_controller.sh \
  "$@"
