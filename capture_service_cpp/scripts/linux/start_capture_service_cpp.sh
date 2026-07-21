#!/usr/bin/env bash
set -euo pipefail

expand_tilde() {
  local value="$1"
  case "$value" in
    "~") printf '%s\n' "$HOME" ;;
    "~/"*) printf '%s\n' "$HOME/${value#"~/"}" ;;
    *) printf '%s\n' "$value" ;;
  esac
}

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BACKEND_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"

# Source tree: BACKEND_DIR=<root>/capture_service_cpp.
# Runtime package: BACKEND_DIR=<root>/bin.
if [[ -n "${ROOT_PROJECT:-}" ]]; then
  ROOT_PROJECT="$(expand_tilde "$ROOT_PROJECT")"
else
  ROOT_PROJECT="$(cd "$BACKEND_DIR/.." && pwd)"
fi

XR_BIN_ROOT="$(expand_tilde "${XR_BIN_ROOT:-$ROOT_PROJECT/bin}")"
BIN="$(expand_tilde "${CAPTURE_SERVICE_CPP_BIN:-$XR_BIN_ROOT/capture_service_cpp/capture_service_cpp}")"

if [[ ! -x "$BIN" ]]; then
  echo "[start_capture_service_cpp][ERROR] capture_service_cpp binary not found: $BIN" >&2
  echo "[start_capture_service_cpp][ERROR] Build/install capture_service_cpp before launching it." >&2
  exit 1
fi

if [[ "${STOP_EXISTING:-1}" == "1" ]]; then
  pkill -TERM -f -- "$BIN" 2>/dev/null || true
  sleep "${STOP_EXISTING_DELAY_SEC:-0.2}"
fi

args=()
CONFIG_PATH="$(expand_tilde "${CONFIG_PATH:-${XR_CAPTURE_CPP_CONFIG:-${CPP_CAPTURE_CONFIG:-}}}")"
CONFIG_DIR="$(expand_tilde "${CONFIG_DIR:-${XR_CAPTURE_CPP_CONFIG_DIR:-${CPP_CAPTURE_CONFIG_DIR:-}}}")"
CONFIG_NAME="${CONFIG_NAME:-${XR_CAPTURE_CPP_CONFIG_NAME:-${CPP_CAPTURE_CONFIG_NAME:-config.yaml}}}"

if [[ -n "$CONFIG_PATH" ]]; then
  args+=(--config "$CONFIG_PATH")
elif [[ -n "$CONFIG_DIR" ]]; then
  args+=(--config-dir "$CONFIG_DIR" --config-name "$CONFIG_NAME")
fi

# Do not inject generic defaults here: the binary and YAML profile already own
# those defaults. Only explicitly exported launcher variables become CLI
# overrides. Device wrappers export their profile-specific values.
if [[ -n "${PUBLISH:-}" ]]; then
  args+=(--publish "$PUBLISH")
fi
if [[ -n "${REGISTRY_PATH:-}" ]]; then
  args+=(--registry "$(expand_tilde "$REGISTRY_PATH")")
fi
CAPTURE_NAMESPACE_VALUE="${CAPTURE_NAMESPACE:-${NAMESPACE:-}}"
if [[ -n "$CAPTURE_NAMESPACE_VALUE" ]]; then
  args+=(--namespace "$CAPTURE_NAMESPACE_VALUE")
fi
CAPTURE_PROFILE_VALUE="${CAPTURE_PROFILE:-${CPP_CAPTURE_PROFILE:-}}"
if [[ -n "$CAPTURE_PROFILE_VALUE" ]]; then
  args+=(--profile "$CAPTURE_PROFILE_VALUE")
fi
if [[ -n "${TCP_BIND_HOST:-}" ]]; then
  args+=(--tcp-bind "$TCP_BIND_HOST")
fi
if [[ -n "${TCP_PORT:-}" ]]; then
  args+=(--tcp-port "$TCP_PORT")
fi

BACKEND_CONTROL_FILE_VALUE="${XR_BACKEND_CONTROL_FILE:-${CPP_CAPTURE_BACKEND_CONTROL_FILE:-}}"
if [[ -n "$BACKEND_CONTROL_FILE_VALUE" ]]; then
  args+=(--backend-control-file "$(expand_tilde "$BACKEND_CONTROL_FILE_VALUE")")
fi
BACKEND_CONTROL_POLL_MS_VALUE="${CAPTURE_SERVICE_BACKEND_CONTROL_POLL_MS:-${CPP_CAPTURE_BACKEND_CONTROL_POLL_MS:-}}"
if [[ -n "$BACKEND_CONTROL_POLL_MS_VALUE" ]]; then
  args+=(--backend-control-poll-ms "$BACKEND_CONTROL_POLL_MS_VALUE")
fi

if [[ "${NO_CAMERA:-0}" == "1" ]]; then
  args+=(--no-camera)
fi
if [[ "${NO_IMU:-0}" == "1" ]]; then
  args+=(--no-imu)
fi
if [[ -n "${DURATION:-}" && "${DURATION}" != "0" ]]; then
  args+=(--duration "$DURATION")
fi

# The caller's CLI options are appended last and therefore remain the
# highest-precedence layer in capture_service_cpp.
echo "[start_capture_service_cpp] $BIN ${args[*]} $*"
exec "$BIN" "${args[@]}" "$@"
