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
export CONFIG_PATH="${CONFIG_PATH:-$CAPTURE_SERVICE_CONFIG_PATH}"
export PUBLISH="${PUBLISH:-shm}"
export REGISTRY_PATH="${REGISTRY_PATH:-$CAPTURE_REGISTRY}"
export TCP_BIND_HOST="${TCP_BIND_HOST:-0.0.0.0}"
export TCP_PORT="${TCP_PORT:-45660}"
export CAPTURE_NAMESPACE="${CAPTURE_NAMESPACE:-xreal_air2ultra_linux}"

BIN="${CAPTURE_SERVICE_CPP_BIN:-$XR_BIN_ROOT/capture_service_cpp/capture_service_cpp}"
if [[ ! -x "$BIN" ]]; then
  echo "[start_capture_service_cpp][ERROR] capture_service_cpp binary not found: $BIN" >&2
  echo "[start_capture_service_cpp][ERROR] Run devices/xreal_ultra/linux/scripts/install_xreal_ultra_out.sh first." >&2
  exit 1
fi

if [[ "${STOP_EXISTING:-1}" == "1" ]]; then
  pkill -TERM -f "${BIN}" 2>/dev/null || true
  sleep 0.2
fi

args=()
if [[ -f "$CONFIG_PATH" ]]; then
  args+=(--config "$CONFIG_PATH")
fi
args+=(
  --publish "$PUBLISH"
  --registry "$REGISTRY_PATH"
  --namespace "$CAPTURE_NAMESPACE"
  --tcp-bind "$TCP_BIND_HOST"
  --tcp-port "$TCP_PORT"
)

if [[ "${NO_CAMERA:-0}" == "1" ]]; then
  args+=(--no-camera)
fi
if [[ "${NO_IMU:-0}" == "1" ]]; then
  args+=(--no-imu)
fi
if [[ -n "${DURATION:-}" && "${DURATION}" != "0" ]]; then
  args+=(--duration "$DURATION")
fi

# Orientation/debug overrides are read by capture_service_cpp from environment
# when supported by the binary.
export XR_CAPTURE_CPP_LEFT_ROTATE="${XR_CAPTURE_CPP_LEFT_ROTATE:-${LEFT_ROTATE:-}}"
export XR_CAPTURE_CPP_RIGHT_ROTATE="${XR_CAPTURE_CPP_RIGHT_ROTATE:-${RIGHT_ROTATE:-}}"
export XR_CAPTURE_CPP_LEFT_FLIP="${XR_CAPTURE_CPP_LEFT_FLIP:-${LEFT_FLIP:-}}"
export XR_CAPTURE_CPP_RIGHT_FLIP="${XR_CAPTURE_CPP_RIGHT_FLIP:-${RIGHT_FLIP:-}}"

echo "[start_capture_service_cpp] $BIN ${args[*]} $*"
exec "$BIN" "${args[@]}" "$@"
