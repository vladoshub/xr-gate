#!/usr/bin/env bash
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEVICE_HOME="$(cd "$SCRIPT_DIR/../../.." && pwd)"
# shellcheck source=/dev/null
source "$DEVICE_HOME/xreal_ultra.env"
export ROOT_PROJECT="${ROOT_PROJECT:-$XR_ROOT_PROJECT}"
SCRIPT="$XR_BIN_ROOT/scripts/runtime_adapters/xr_runtime_adapter/start_xr_runtime_adapter_shm.sh"
if [[ ! -x "$SCRIPT" ]]; then
  SCRIPT="$XR_ROOT_PROJECT/runtime_adapters/xr_runtime_adapter/scripts/linux/start_xr_runtime_adapter_shm.sh"
fi
exec "$SCRIPT" "$@"
