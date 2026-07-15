#!/usr/bin/env bash
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=/dev/null
source "$SCRIPT_DIR/../lib/device_runtime.sh"
xr_load_device_env

impl="${CAPTURE_SERVICE_IMPL:-cpp}"
case "${impl,,}" in
  cpp|native|capture_service_cpp)
    exec "$SCRIPT_DIR/start_capture_service_cpp.sh" "$@"
    ;;
  python|py|gstreamer)
    xr_common_fatal "legacy Python/GStreamer capture_service was removed; use CAPTURE_SERVICE_IMPL=cpp"
    ;;
  *)
    xr_common_fatal "unsupported CAPTURE_SERVICE_IMPL=$impl; expected cpp"
    ;;
esac
