#!/usr/bin/env bash
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEVICE_HOME="$(cd "$SCRIPT_DIR/../../.." && pwd)"
PACKAGE_ROOT="$(cd "$DEVICE_HOME/../.." && pwd)"
export XR_DEVICE_ENV="${XR_DEVICE_ENV:-$DEVICE_HOME/xreal_ultra.env}"
exec "$PACKAGE_ROOT/devices/common/linux/scripts/mercury_hand_tracking/start_hand_tracking.sh" "$@"
