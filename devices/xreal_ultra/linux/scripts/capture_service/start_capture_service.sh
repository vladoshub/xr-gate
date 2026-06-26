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

impl="${CAPTURE_SERVICE_IMPL:-cpp}"
case "${impl,,}" in
  cpp|native|capture_service_cpp)
    exec "$SCRIPT_DIR/start_capture_service_cpp.sh" "$@"
    ;;
  python|py|gstreamer)
    echo "[start_capture_service][ERROR] legacy Python/GStreamer capture_service was removed from this package." >&2
    echo "[start_capture_service][ERROR] Use CAPTURE_SERVICE_IMPL=cpp or restore/build the legacy component separately." >&2
    exit 2
    ;;
  *)
    echo "[start_capture_service][ERROR] unsupported CAPTURE_SERVICE_IMPL=$impl; expected cpp" >&2
    exit 2
    ;;
esac
