#!/usr/bin/env bash
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEVICE_HOME="$(cd "$SCRIPT_DIR/../../.." && pwd)"
# shellcheck source=/dev/null
source "$DEVICE_HOME/xreal_ultra.env"
export ROOT_PROJECT="${ROOT_PROJECT:-$XR_ROOT_PROJECT}"
SCRIPT="$XR_BIN_ROOT/scripts/backends/imu_3dof/start_imu_3dof_backend.sh"
if [[ ! -x "$SCRIPT" ]]; then
  SCRIPT="$XR_ROOT_PROJECT/backends/imu_3dof/scripts/linux/start_imu_3dof_backend.sh"
fi
exec "$SCRIPT" "$@"
