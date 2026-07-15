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
ROOT_PROJECT="${ROOT_PROJECT:-$HOME/src/xr_tracking}"
ROOT_PROJECT="$(expand_tilde "$ROOT_PROJECT")"

TRANSPORT="${TRANSPORT:-shm}"
REGISTRY="${REGISTRY:-/tmp/capture_service_streams.json}"
CAPTURE_TCP_HOST="${CAPTURE_TCP_HOST:-127.0.0.1}"
CAPTURE_TCP_PORT="${CAPTURE_TCP_PORT:-45660}"
CAM0_STREAM="${CAM0_STREAM:-camera0}"
CAM1_STREAM="${CAM1_STREAM:-camera1}"

# Optional TCP metadata probe for automatic profile selection. Disabled by
# default so the existing local-registry/XREAL path is unchanged.
CAPTURE_PROFILE_PROBE_ENABLED="${CAPTURE_PROFILE_PROBE_ENABLED:-0}"
CAPTURE_PROFILE_PROBE_HOST="${CAPTURE_PROFILE_PROBE_HOST:-$CAPTURE_TCP_HOST}"
CAPTURE_PROFILE_PROBE_PORT="${CAPTURE_PROFILE_PROBE_PORT:-$CAPTURE_TCP_PORT}"
CAPTURE_PROFILE_PROBE_TIMEOUT_MS="${CAPTURE_PROFILE_PROBE_TIMEOUT_MS:-1500}"

XR_CALIB_DIR="${XR_CALIB_DIR:-$ROOT_PROJECT/calibration_dataset}"
XR_CALIB_DIR="$(expand_tilde "$XR_CALIB_DIR")"

PROFILE_HELPER=""
for candidate in \
  "$SCRIPT_DIR/capture_profile.sh" \
  "$ROOT_PROJECT/backends/common/scripts/linux/capture_profile.sh" \
  "$ROOT_PROJECT/bin/backends/mercury_hand_tracking/scripts/linux/capture_profile.sh"
do
  if [[ -f "$candidate" ]]; then PROFILE_HELPER="$candidate"; break; fi
done
[[ -n "$PROFILE_HELPER" ]] || {
  echo "[start_hand_tracking][ERROR] capture_profile.sh not found" >&2
  exit 2
}
# shellcheck source=/dev/null
source "$PROFILE_HELPER"
capture_profile_parse_cli "$@"
set -- "${CAPTURE_PROFILE_FORWARD_ARGS[@]}"
capture_profile_load_backend \
  "mercury_hand_tracking" \
  "${CAPTURE_PROFILE_CLI_OVERRIDE:-${MERCURY_PROFILE:-${CAPTURE_PROFILE:-}}}" \
  "$REGISTRY" \
  "xreal_air2ultra_unified_480" \
  "$ROOT_PROJECT" \
  "$SCRIPT_DIR" \
  "${MERCURY_PROFILE_DIR:-}"

XR_DEVICE_NAME="${XR_DEVICE_NAME:-xreal_air2ultra}"
XR_SERIAL="${XR_SERIAL:-ZBBM5DZFMP}"
CALIB_PROFILE_NAME="${CALIB_PROFILE_NAME:-unified_480_ccw90}"
FINAL="${FINAL:-$XR_CALIB_DIR/final/$XR_DEVICE_NAME/$XR_SERIAL/$CALIB_PROFILE_NAME}"
FINAL="$(expand_tilde "$FINAL")"
MERCURY_CALIB="${MERCURY_CALIB:-$FINAL/mercury_calib_unified_480_ccw90.json}"
MERCURY_CALIB="$(expand_tilde "$MERCURY_CALIB")"

ORT_ROOT="${ORT_ROOT:-$ROOT_PROJECT/bin/onnxruntime/onnxruntime-linux-x64-1.18.1}"
ORT_ROOT="$(expand_tilde "$ORT_ROOT")"
export LD_LIBRARY_PATH="$ORT_ROOT/lib:${LD_LIBRARY_PATH:-}"

INSTALL_BIN_DIR="${INSTALL_BIN_DIR:-$ROOT_PROJECT/bin/backends/mercury_hand_tracking}"
INSTALL_BIN_DIR="$(expand_tilde "$INSTALL_BIN_DIR")"
MERCURY_LIB="${MERCURY_LIB:-$INSTALL_BIN_DIR/libxr_mercury_runtime.so}"
BACKEND_BIN="${BACKEND_BIN:-$INSTALL_BIN_DIR/capture_hand_tracking_backend}"
MERCURY_MODELS="${MERCURY_MODELS:-$ROOT_PROJECT/bin/hand-tracking-models/mercury}"
MERCURY_LIB="$(expand_tilde "$MERCURY_LIB")"
BACKEND_BIN="$(expand_tilde "$BACKEND_BIN")"
MERCURY_MODELS="$(expand_tilde "$MERCURY_MODELS")"

DURATION="${DURATION:-0}"
PRINT_EVERY="${PRINT_EVERY:-30}"
MERCURY_MIN_DETECTION_CONFIDENCE="${MERCURY_MIN_DETECTION_CONFIDENCE:-0.3}"
# Keep Mercury backend raw by default. Runtime-side filtering now lives in
# only for rollback/debug comparisons.

#export MERCURY_XR_DEBUG_DUMP_DIR="${MERCURY_XR_DEBUG_DUMP_DIR:-/tmp/xr_mercury_debug}"
export MERCURY_XR_DEBUG_DUMP_LATEST_ONLY="${MERCURY_XR_DEBUG_DUMP_LATEST_ONLY:-1}"
export MERCURY_XR_DEBUG_DUMP_EVERY_N="${MERCURY_XR_DEBUG_DUMP_EVERY_N:-1}"

echo "[start_hand_tracking] capture_profile=$CAPTURE_PROFILE_RESOLVED"
echo "[start_hand_tracking] capture_profile_source=$CAPTURE_PROFILE_SOURCE_RESOLVED"
echo "[start_hand_tracking] profile_file=$CAPTURE_PROFILE_FILE_RESOLVED"

ls -lh \
  "$BACKEND_BIN" \
  "$MERCURY_LIB" \
  "$MERCURY_CALIB" \
  "$MERCURY_MODELS/grayscale_detection_160x160.onnx" \
  "$MERCURY_MODELS/grayscale_keypoint_jan18.onnx"

args=(
  --transport "$TRANSPORT"
  --registry "$REGISTRY"
  --tcp-host "$CAPTURE_TCP_HOST"
  --tcp-port "$CAPTURE_TCP_PORT"
  --cam0-stream "$CAM0_STREAM"
  --cam1-stream "$CAM1_STREAM"
  --duration "$DURATION"
  --hand-format-version 2
  --hand-tracker mercury
  --mercury-runtime-lib "$MERCURY_LIB"
  --mercury-models "$MERCURY_MODELS"
  --mercury-calib "$MERCURY_CALIB"
  --mercury-min-detection-confidence "$MERCURY_MIN_DETECTION_CONFIDENCE"
  --print-every "$PRINT_EVERY"
)


exec "$BACKEND_BIN" "${args[@]}" "$@"
