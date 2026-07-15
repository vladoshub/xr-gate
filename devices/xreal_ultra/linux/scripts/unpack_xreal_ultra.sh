#!/usr/bin/env bash
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
XR_ROOT_PROJECT="${XR_ROOT_PROJECT:-$(cd "$SCRIPT_DIR/../../../.." && pwd)}"
export XR_RELEASE_DEVICE_TARGET="${XR_RELEASE_DEVICE_TARGET:-xreal_ultra}"
export XR_RELEASE_DEVICE_DISPLAY_NAME="${XR_RELEASE_DEVICE_DISPLAY_NAME:-XREAL Ultra}"
export XR_RELEASE_DEVICE_ENV_NAME="${XR_RELEASE_DEVICE_ENV_NAME:-xreal_ultra.env}"
exec "$XR_ROOT_PROJECT/devices/common/linux/scripts/release/unpack_device_release.sh" "$@"
