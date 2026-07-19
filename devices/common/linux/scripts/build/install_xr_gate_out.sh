#!/usr/bin/env bash
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=/dev/null
source "$SCRIPT_DIR/xr_gate_out_env.sh"
export XR_DEVICE_PACKAGE_SCRIPT="${XR_DEVICE_PACKAGE_SCRIPT:-$SCRIPT_DIR/package_device_out.sh}"
exec "$SCRIPT_DIR/install_device_out.sh" "$@"
