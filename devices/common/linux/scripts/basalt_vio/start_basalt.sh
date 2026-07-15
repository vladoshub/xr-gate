#!/usr/bin/env bash
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=/dev/null
source "$SCRIPT_DIR/../lib/device_runtime.sh"
xr_load_device_env
xr_exec_runtime_script basalt_vio \
  scripts/backends/basalt_vio/start_basalt.sh \
  backends/basalt_vio/scripts/linux/start_basalt.sh \
  "$@"
