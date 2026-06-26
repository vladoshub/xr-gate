#!/usr/bin/env bash
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEVICE_HOME="$(cd "$SCRIPT_DIR/../../.." && pwd)"
# shellcheck source=/dev/null
source "$DEVICE_HOME/xreal_ultra.env"
SCRIPT="$XR_MONADO_START_SCRIPT"
if [[ ! -x "$SCRIPT" ]]; then
  SCRIPT="$XR_BIN_ROOT/scripts/drivers/monado_driver/start.sh"
fi
exec "$SCRIPT" "$@"
