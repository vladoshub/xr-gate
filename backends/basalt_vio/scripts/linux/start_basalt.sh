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
ROOT_PROJECT="${ROOT_PROJECT:-${XR:-$HOME/src/xr_tracking}}"
ROOT_PROJECT="$(expand_tilde "$ROOT_PROJECT")"

BASALT_BIN_DIR="${BASALT_BIN_DIR:-$ROOT_PROJECT/bin/backends/basalt_vio}"
BASALT_BIN_DIR="$(expand_tilde "$BASALT_BIN_DIR")"
BASALT_LIB_DIR="${BASALT_LIB_DIR:-$BASALT_BIN_DIR/lib}"
BASALT_LIB_DIR="$(expand_tilde "$BASALT_LIB_DIR")"

if [[ -d "$BASALT_LIB_DIR" ]]; then
  export LD_LIBRARY_PATH="$BASALT_LIB_DIR:${LD_LIBRARY_PATH:-}"
fi

CAPTURE_REGISTRY="${CAPTURE_REGISTRY:-/tmp/capture_service_streams.json}"
CAM0_STREAM="${CAM0_STREAM:-camera0}"
CAM1_STREAM="${CAM1_STREAM:-camera1}"
IMU_STREAM="${IMU_STREAM:-imu0}"

XR_CALIB_DIR="${XR_CALIB_DIR:-$ROOT_PROJECT/calibration_dataset}"
XR_CALIB_DIR="$(expand_tilde "$XR_CALIB_DIR")"

PROFILE_HELPER=""
for candidate in \
  "$SCRIPT_DIR/capture_profile.sh" \
  "$ROOT_PROJECT/backends/common/scripts/linux/capture_profile.sh" \
  "$ROOT_PROJECT/bin/backends/basalt_vio/scripts/linux/capture_profile.sh"
do
  if [[ -f "$candidate" ]]; then PROFILE_HELPER="$candidate"; break; fi
done
[[ -n "$PROFILE_HELPER" ]] || {
  echo "[start_basalt][ERROR] capture_profile.sh not found" >&2
  exit 2
}
# shellcheck source=/dev/null
source "$PROFILE_HELPER"
capture_profile_load_backend \
  "basalt_vio" \
  "${BASALT_PROFILE:-${CAPTURE_PROFILE:-}}" \
  "$CAPTURE_REGISTRY" \
  "xreal_air2ultra_unified_480" \
  "$ROOT_PROJECT" \
  "$SCRIPT_DIR" \
  "${BASALT_PROFILE_DIR:-}"

if [[ "${BASALT_PROFILE_SUPPORTED:-1}" != "1" ]]; then
  echo "[start_basalt][ERROR] capture profile '$CAPTURE_PROFILE_RESOLVED' is not supported by Basalt: ${BASALT_PROFILE_UNSUPPORTED_REASON:-IMU is required}" >&2
  exit 2
fi

XR_SERIAL="${XR_SERIAL:-ZBBM5DZFMP}"
CALIB_PROFILE_NAME="${CALIB_PROFILE_NAME:-unified_480_ccw90}"
XR_DEVICE_NAME="${XR_DEVICE_NAME:-xreal_air2ultra}"
FINAL_PROFILE_DIR="${FINAL_PROFILE_DIR:-$XR_CALIB_DIR/final/$XR_DEVICE_NAME/$XR_SERIAL/$CALIB_PROFILE_NAME}"
FINAL_PROFILE_DIR="$(expand_tilde "$FINAL_PROFILE_DIR")"
BASALT_CALIB="${BASALT_CALIB:-$FINAL_PROFILE_DIR/basalt_calib_unified_480_ccw90.json}"
BASALT_VIO_CONFIG="${BASALT_VIO_CONFIG:-$FINAL_PROFILE_DIR/basalt_vio_config_unified_480_ccw90.json}"
BASALT_CALIB="$(expand_tilde "$BASALT_CALIB")"
BASALT_VIO_CONFIG="$(expand_tilde "$BASALT_VIO_CONFIG")"

OUT_DIR="${OUT_DIR:-/tmp/xr_basalt_unified_live}"
XR_BACKEND_CONTROL_FILE="${XR_BACKEND_CONTROL_FILE:-/tmp/xr_backend_control.json}"
export XR_BACKEND_CONTROL_FILE

STARTUP_GATE="${STARTUP_GATE:-0}"
STARTUP_GATE_SCRIPT="${STARTUP_GATE_SCRIPT:-$ROOT_PROJECT/tools/xr_startup_gate.py}"
STARTUP_GATE_SCRIPT="$(expand_tilde "$STARTUP_GATE_SCRIPT")"
STARTUP_GATE_TIMEOUT_SEC="${STARTUP_GATE_TIMEOUT_SEC:-0}"
STARTUP_GATE_PRINT_EVERY="${STARTUP_GATE_PRINT_EVERY:-5}"
STARTUP_GATE_VISUAL="${STARTUP_GATE_VISUAL:-1}"
STARTUP_GATE_IMU="${STARTUP_GATE_IMU:-1}"
STARTUP_VISUAL_GOOD_FRAMES="${STARTUP_VISUAL_GOOD_FRAMES:-30}"
STARTUP_MIN_MEAN="${STARTUP_MIN_MEAN:-22}"
STARTUP_MIN_STDDEV="${STARTUP_MIN_STDDEV:-10}"
STARTUP_MAX_BLACK_FRACTION="${STARTUP_MAX_BLACK_FRACTION:-0.60}"
STARTUP_MAX_WHITE_FRACTION="${STARTUP_MAX_WHITE_FRACTION:-0.15}"
STARTUP_MIN_CORNERS="${STARTUP_MIN_CORNERS:-200}"
STARTUP_MIN_GRID_CELLS="${STARTUP_MIN_GRID_CELLS:-10}"
STARTUP_MIN_LAPLACIAN_STDDEV="${STARTUP_MIN_LAPLACIAN_STDDEV:-16}"
STARTUP_IMU_GOOD_FRAMES="${STARTUP_IMU_GOOD_FRAMES:-30}"
STARTUP_IMU_MIN_SAMPLES="${STARTUP_IMU_MIN_SAMPLES:-10}"
STARTUP_IMU_MAX_GYRO_NORM="${STARTUP_IMU_MAX_GYRO_NORM:-0.08}"
STARTUP_IMU_MAX_GYRO_STDDEV="${STARTUP_IMU_MAX_GYRO_STDDEV:-0.04}"
STARTUP_IMU_MAX_ACCEL_MAGNITUDE_ERROR="${STARTUP_IMU_MAX_ACCEL_MAGNITUDE_ERROR:-0.75}"
STARTUP_IMU_MAX_ACCEL_STDDEV="${STARTUP_IMU_MAX_ACCEL_STDDEV:-0.35}"
GRAVITY_MAGNITUDE="${GRAVITY_MAGNITUDE:-9.80665}"

mkdir -p "$OUT_DIR"

echo "[start_basalt] ROOT_PROJECT=$ROOT_PROJECT"
echo "[start_basalt] CAPTURE_PROFILE=$CAPTURE_PROFILE_RESOLVED"
echo "[start_basalt] PROFILE_FILE=$CAPTURE_PROFILE_FILE_RESOLVED"
echo "[start_basalt] BASALT_BIN_DIR=$BASALT_BIN_DIR"
echo "[start_basalt] BASALT_LIB_DIR=$BASALT_LIB_DIR"
echo "[start_basalt] FINAL_PROFILE_DIR=$FINAL_PROFILE_DIR"
echo "[start_basalt] BASALT_CALIB=$BASALT_CALIB"
echo "[start_basalt] BASALT_VIO_CONFIG=$BASALT_VIO_CONFIG"
echo "[start_basalt] XR_BACKEND_CONTROL_FILE=$XR_BACKEND_CONTROL_FILE"
echo "[start_basalt] STARTUP_GATE=$STARTUP_GATE"
echo "[start_basalt] STARTUP_GATE_SCRIPT=$STARTUP_GATE_SCRIPT"
echo "[start_basalt] LD_LIBRARY_PATH=${LD_LIBRARY_PATH:-}"

if [[ "$STARTUP_GATE" == "1" ]]; then
  gate_args=(
    --transport shm
    --registry "$CAPTURE_REGISTRY"
    --cam0-stream "$CAM0_STREAM"
    --cam1-stream "$CAM1_STREAM"
    --imu-stream "$IMU_STREAM"
    --timeout-s "$STARTUP_GATE_TIMEOUT_SEC"
    --print-every "$STARTUP_GATE_PRINT_EVERY"
    --gravity-magnitude "$GRAVITY_MAGNITUDE"
  )

  if [[ "$STARTUP_GATE_VISUAL" == "1" ]]; then
    gate_args+=(
      --visual-gate
      --visual-good-frames "$STARTUP_VISUAL_GOOD_FRAMES"
      --min-mean "$STARTUP_MIN_MEAN"
      --min-stddev "$STARTUP_MIN_STDDEV"
      --max-black-fraction "$STARTUP_MAX_BLACK_FRACTION"
      --max-white-fraction "$STARTUP_MAX_WHITE_FRACTION"
      --min-corners "$STARTUP_MIN_CORNERS"
      --min-grid-cells "$STARTUP_MIN_GRID_CELLS"
      --min-laplacian-stddev "$STARTUP_MIN_LAPLACIAN_STDDEV"
    )
  fi

  if [[ "$STARTUP_GATE_IMU" == "1" ]]; then
    gate_args+=(
      --imu-gate
      --imu-good-frames "$STARTUP_IMU_GOOD_FRAMES"
      --imu-min-samples "$STARTUP_IMU_MIN_SAMPLES"
      --imu-max-gyro-norm "$STARTUP_IMU_MAX_GYRO_NORM"
      --imu-max-gyro-stddev "$STARTUP_IMU_MAX_GYRO_STDDEV"
      --imu-max-accel-magnitude-error "$STARTUP_IMU_MAX_ACCEL_MAGNITUDE_ERROR"
      --imu-max-accel-stddev "$STARTUP_IMU_MAX_ACCEL_STDDEV"
    )
  fi

  echo "[start_basalt] Running external startup gate"
  PYTHONPATH="$ROOT_PROJECT/capture_service:${PYTHONPATH:-}" \
    python3 "$STARTUP_GATE_SCRIPT" "${gate_args[@]}"
fi

exec "$BASALT_BIN_DIR/capture_basalt_backend" \
  --transport shm \
  --registry "$CAPTURE_REGISTRY" \
  --cam0-stream "$CAM0_STREAM" \
  --cam1-stream "$CAM1_STREAM" \
  --imu-stream "$IMU_STREAM" \
  --cam-calib "$BASALT_CALIB" \
  --config-path "$BASALT_VIO_CONFIG" \
  --out-dir "$OUT_DIR" \
  --duration 0 \
  --image-scale 256 \
  --no-enforce-realtime \
  "$@"
