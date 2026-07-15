#!/usr/bin/env bash
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=/dev/null
source "$SCRIPT_DIR/../lib/device_runtime.sh"
xr_load_device_env

PY_RUNTIME_ENV="$XR_PACKAGE_ROOT/bin/python-runtime/env.sh"
if [[ -f "$PY_RUNTIME_ENV" ]]; then
  # shellcheck source=/dev/null
  source "$PY_RUNTIME_ENV"
fi
: "${XR_RUNTIME_DEBUG_VIEWER_CONFIG:?device env must define XR_RUNTIME_DEBUG_VIEWER_CONFIG}"
exec "${PYTHON:-python3}" "$XR_TOOLS_DIR/runtime_debug_viewer/xr_runtime_debug_viewer.py" \
  --config "$XR_RUNTIME_DEBUG_VIEWER_CONFIG" "$@"
