#!/usr/bin/env bash
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEVICE_HOME="$(cd "$SCRIPT_DIR/../../.." && pwd)"
# shellcheck source=/dev/null
source "$DEVICE_HOME/xreal_ultra.env"

export ROOT_PROJECT="${ROOT_PROJECT:-$XR_ROOT_PROJECT}"
export CONFIG_PATH="${CONFIG_PATH:-$CAPTURE_SERVICE_CONFIG_PATH}"
export PUBLISH="${PUBLISH:-shm}"
export REGISTRY_PATH="${REGISTRY_PATH:-$CAPTURE_REGISTRY}"
export TCP_BIND_HOST="${TCP_BIND_HOST:-0.0.0.0}"
export TCP_PORT="${TCP_PORT:-45660}"
export CAPTURE_NAMESPACE="${CAPTURE_NAMESPACE:-$CAPTURE_REGISTRY_NAMESPACE}"

# Device-specific orientation/debug compatibility overrides. The generic
# launcher does not know anything about XREAL or its coordinate convention.
export XR_CAPTURE_CPP_LEFT_ROTATE="${XR_CAPTURE_CPP_LEFT_ROTATE:-${LEFT_ROTATE:-}}"
export XR_CAPTURE_CPP_RIGHT_ROTATE="${XR_CAPTURE_CPP_RIGHT_ROTATE:-${RIGHT_ROTATE:-}}"
export XR_CAPTURE_CPP_LEFT_FLIP="${XR_CAPTURE_CPP_LEFT_FLIP:-${LEFT_FLIP:-}}"
export XR_CAPTURE_CPP_RIGHT_FLIP="${XR_CAPTURE_CPP_RIGHT_FLIP:-${RIGHT_FLIP:-}}"

START_SCRIPT="$XR_BIN_ROOT/scripts/capture_service_cpp/start_capture_service_cpp.sh"
if [[ ! -x "$START_SCRIPT" ]]; then
  START_SCRIPT="$XR_ROOT_PROJECT/capture_service_cpp/scripts/linux/start_capture_service_cpp.sh"
fi
if [[ ! -x "$START_SCRIPT" ]]; then
  echo "[device/start_capture_service_cpp][ERROR] start script not found: $START_SCRIPT" >&2
  echo "[device/start_capture_service_cpp][ERROR] Build/package capture_service_cpp first." >&2
  exit 2
fi

exec "$START_SCRIPT" "$@"
