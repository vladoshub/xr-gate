#!/usr/bin/env bash
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=/dev/null
source "$SCRIPT_DIR/../lib/device_runtime.sh"
xr_load_device_env

: "${CAPTURE_SERVICE_CONFIG_PATH:?device env must define CAPTURE_SERVICE_CONFIG_PATH}"
export CONFIG_PATH="${CONFIG_PATH:-$CAPTURE_SERVICE_CONFIG_PATH}"
export PUBLISH="${PUBLISH:-shm}"
export REGISTRY_PATH="${REGISTRY_PATH:-$CAPTURE_REGISTRY}"
export TCP_BIND_HOST="${TCP_BIND_HOST:-0.0.0.0}"
export TCP_PORT="${TCP_PORT:-45660}"
export CAPTURE_NAMESPACE="${CAPTURE_NAMESPACE:-${CAPTURE_REGISTRY_NAMESPACE:-$XR_TARGET_DEVICE}}"

# Optional compatibility transforms remain device-provided environment values.
export XR_CAPTURE_CPP_LEFT_ROTATE="${XR_CAPTURE_CPP_LEFT_ROTATE:-${LEFT_ROTATE:-}}"
export XR_CAPTURE_CPP_RIGHT_ROTATE="${XR_CAPTURE_CPP_RIGHT_ROTATE:-${RIGHT_ROTATE:-}}"
export XR_CAPTURE_CPP_LEFT_FLIP="${XR_CAPTURE_CPP_LEFT_FLIP:-${LEFT_FLIP:-}}"
export XR_CAPTURE_CPP_RIGHT_FLIP="${XR_CAPTURE_CPP_RIGHT_FLIP:-${RIGHT_FLIP:-}}"

xr_exec_runtime_script capture_service_cpp \
  scripts/capture_service_cpp/start_capture_service_cpp.sh \
  capture_service_cpp/scripts/linux/start_capture_service_cpp.sh \
  "$@"
