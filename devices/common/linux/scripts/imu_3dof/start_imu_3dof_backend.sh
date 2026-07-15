#!/usr/bin/env bash
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=/dev/null
source "$SCRIPT_DIR/../lib/device_runtime.sh"
xr_load_device_env
xr_exec_runtime_script imu_3dof \
  scripts/backends/imu_3dof/start_imu_3dof_backend.sh \
  backends/imu_3dof/scripts/linux/start_imu_3dof_backend.sh \
  "$@"
