#!/usr/bin/env bash
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=/dev/null
source "$SCRIPT_DIR/../lib/device_runtime.sh"
xr_load_device_env
SCRIPT="${XR_MONADO_START_SCRIPT:-$XR_BIN_ROOT/drivers/monado_driver/start.sh}"
[[ -x "$SCRIPT" ]] || xr_common_fatal "Monado start script not found: $SCRIPT"
exec "$SCRIPT" "$@"
