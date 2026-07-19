#!/usr/bin/env bash
# Compatibility entrypoint. It now builds the generic multi-profile XR Gate
# package instead of an XREAL-only package.
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
XR_ROOT_PROJECT="${XR_ROOT_PROJECT:-$(cd "$SCRIPT_DIR/../../../.." && pwd)}"
export XR_ROOT_PROJECT
exec "$XR_ROOT_PROJECT/devices/common/linux/scripts/build/install_xr_gate_out.sh" "$@"
