#!/usr/bin/env bash
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEVICE_HOME="$(cd "$SCRIPT_DIR/../../.." && pwd)"
PACKAGE_ROOT="$(cd "$DEVICE_HOME/../.." && pwd)"
# shellcheck source=/dev/null
source "$DEVICE_HOME/xreal_ultra.env"
PY_RUNTIME_ENV="$PACKAGE_ROOT/bin/python-runtime/env.sh"
if [[ -f "$PY_RUNTIME_ENV" ]]; then
  # shellcheck source=/dev/null
  source "$PY_RUNTIME_ENV"
fi
export ROOT_PROJECT="${ROOT_PROJECT:-$XR_ROOT_PROJECT}"
exec "${PYTHON:-python3}" "$XR_TOOLS_DIR/runtime_debug_viewer/xr_runtime_debug_viewer.py" --config "${XR_RUNTIME_DEBUG_VIEWER_CONFIG}" "$@"
