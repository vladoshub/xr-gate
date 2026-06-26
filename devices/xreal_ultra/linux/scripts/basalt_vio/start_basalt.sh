#!/usr/bin/env bash
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEVICE_HOME="$(cd "$SCRIPT_DIR/../../.." && pwd)"
# shellcheck source=/dev/null
source "$DEVICE_HOME/xreal_ultra.env"
export ROOT_PROJECT="${ROOT_PROJECT:-$XR_ROOT_PROJECT}"
SCRIPT="$XR_BIN_ROOT/scripts/backends/basalt_vio/start_basalt.sh"
if [[ ! -x "$SCRIPT" ]]; then
  SCRIPT="$XR_ROOT_PROJECT/backends/basalt_vio/scripts/linux/start_basalt.sh"
fi
exec "$SCRIPT" "$@"
