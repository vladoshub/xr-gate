#!/usr/bin/env bash
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEVICE_HOME="$(cd "$SCRIPT_DIR/../../.." && pwd)"
PACKAGE_ROOT="$(cd "$DEVICE_HOME/../.." && pwd)"
export XR_DEVICE_ENV="${XR_DEVICE_ENV:-$DEVICE_HOME/xreal_ultra.env}"
exec "$PACKAGE_ROOT/devices/common/linux/scripts/imu_3dof/start_imu_3dof_backend.sh" "$@"
