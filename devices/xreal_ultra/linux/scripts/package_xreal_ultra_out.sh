#!/usr/bin/env bash
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=/dev/null
source "$SCRIPT_DIR/xreal_ultra_out_env.sh"
export XR_DEVICE_PACKAGE_HOOK="${XR_DEVICE_PACKAGE_HOOK:-$SCRIPT_DIR/package_xreal_ultra_components.sh}"
exec "$XR_ROOT_PROJECT/devices/common/linux/scripts/build/package_device_out.sh" "$@"
