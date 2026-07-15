#!/usr/bin/env bash
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=/dev/null
source "$SCRIPT_DIR/../lib/device_runtime.sh"
xr_load_device_env
xr_exec_runtime_script xr_runtime_adapter \
  scripts/runtime_adapters/xr_runtime_adapter/start_xr_runtime_adapter_shm.sh \
  runtime_adapters/xr_runtime_adapter/scripts/linux/start_xr_runtime_adapter_shm.sh \
  "$@"
