#!/usr/bin/env bash
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export XR_RELEASE_DEVICE_TARGET="${XR_RELEASE_DEVICE_TARGET:-xr-gate}"
export XR_RELEASE_DEVICE_DISPLAY_NAME="${XR_RELEASE_DEVICE_DISPLAY_NAME:-XR Gate}"
export XR_RELEASE_REQUIRE_DEVICE="${XR_RELEASE_REQUIRE_DEVICE:-0}"
exec "$SCRIPT_DIR/unpack_device_release.sh" "$@"
