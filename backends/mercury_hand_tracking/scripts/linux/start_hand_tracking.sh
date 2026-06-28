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

ROOT_PROJECT="${ROOT_PROJECT:-$HOME/src/xr_tracking}"
ROOT_PROJECT="$(expand_tilde "$ROOT_PROJECT")"

XR_CALIB_DIR="${XR_CALIB_DIR:-$ROOT_PROJECT/calibration_dataset}"
XR_CALIB_DIR="$(expand_tilde "$XR_CALIB_DIR")"
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

TRANSPORT="${TRANSPORT:-shm}"
REGISTRY="${REGISTRY:-/tmp/capture_service_streams.json}"
CAM0_STREAM="${CAM0_STREAM:-camera0}"
CAM1_STREAM="${CAM1_STREAM:-camera1}"
DURATION="${DURATION:-0}"
PRINT_EVERY="${PRINT_EVERY:-30}"
MERCURY_MIN_DETECTION_CONFIDENCE="${MERCURY_MIN_DETECTION_CONFIDENCE:-0.03}"
# Keep Mercury backend raw by default. Runtime-side filtering now lives in
# only for rollback/debug comparisons.

#export MERCURY_XR_DEBUG_DUMP_DIR="${MERCURY_XR_DEBUG_DUMP_DIR:-/tmp/xr_mercury_debug}"
export MERCURY_XR_DEBUG_DUMP_LATEST_ONLY="${MERCURY_XR_DEBUG_DUMP_LATEST_ONLY:-1}"
export MERCURY_XR_DEBUG_DUMP_EVERY_N="${MERCURY_XR_DEBUG_DUMP_EVERY_N:-1}"

ls -lh \
  "$BACKEND_BIN" \
  "$MERCURY_LIB" \
  "$MERCURY_CALIB" \
  "$MERCURY_MODELS/grayscale_detection_160x160.onnx" \
  "$MERCURY_MODELS/grayscale_keypoint_jan18.onnx"

args=(
  --transport "$TRANSPORT"
  --registry "$REGISTRY"
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


exec "$BACKEND_BIN" "${args[@]}"
