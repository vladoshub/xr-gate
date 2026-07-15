#!/usr/bin/env bash
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
XR_PACKAGE_ROOT="${XR_PACKAGE_ROOT:-$(cd "$SCRIPT_DIR/../../../.." && pwd)}"
export XR_PACKAGE_ROOT
export XR_TARGET_DEVICE="${XR_TARGET_DEVICE:-xreal_ultra}"
export XR_RUNTIME_DEVICE_ACCESS_SCRIPT="${XR_RUNTIME_DEVICE_ACCESS_SCRIPT:-$SCRIPT_DIR/install_xreal_ultra_device_access.sh}"
exec "$XR_PACKAGE_ROOT/devices/common/linux/scripts/runtime/install_runtime_deps_ubuntu24.sh" "$@"
