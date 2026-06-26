#!/usr/bin/env bash
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEVICE_HOME="$(cd "$SCRIPT_DIR/../../.." && pwd)"
# shellcheck source=/dev/null
source "$DEVICE_HOME/xreal_ultra.env"
export ROOT_PROJECT="${ROOT_PROJECT:-$XR_ROOT_PROJECT}"
export BIN_DIR="${BIN_DIR:-$BIN_DIR_XR_SPATIAL}"
SCRIPT="$XR_BIN_ROOT/scripts/backends/xr_spatial/start_xr_spatial_scan.sh"
if [[ ! -x "$SCRIPT" ]]; then
  SCRIPT="$XR_ROOT_PROJECT/backends/xr_spatial/scripts/linux/start_xr_spatial_scan.sh"
fi
exec "$SCRIPT" "$@"
