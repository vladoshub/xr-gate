#!/usr/bin/env bash
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=/dev/null
source "$SCRIPT_DIR/../lib/device_runtime.sh"
xr_load_device_env
export BIN_DIR="${BIN_DIR:-$BIN_DIR_XR_SPATIAL}"
xr_exec_runtime_script xr_spatial_tcp \
  scripts/backends/xr_spatial/start_xr_spatial_tcp.sh \
  backends/xr_spatial/scripts/linux/start_xr_spatial_tcp.sh \
  "$@"
