#!/usr/bin/env bash
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEVICE_HOME="$(cd "$SCRIPT_DIR/../../.." && pwd)"
# shellcheck source=/dev/null
source "$DEVICE_HOME/xreal_ultra.env"
export ROOT_PROJECT="${ROOT_PROJECT:-$XR_ROOT_PROJECT}"
exec "$XREAL_DISPLAY_HELPER_BIN" --mode 60hz --keep-running "$@"
