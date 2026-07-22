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

validate_basalt_json_object() {
  local label="$1"
  local path="$2"
  local json_python="${PYTHON:-python3}"

  if [[ ! -s "$path" ]]; then
    echo "[start_basalt][ERROR] $label JSON is missing or empty: $path" >&2
    return 1
  fi

  if ! command -v "$json_python" >/dev/null 2>&1; then
    echo "[start_basalt][ERROR] Python interpreter not found for JSON validation: $json_python" >&2
    return 1
  fi

  "$json_python" - "$label" "$path" <<'PY_JSON_CHECK'
import json
import sys

label, path = sys.argv[1:]
try:
    with open(path, "r", encoding="utf-8") as stream:
        document = json.load(stream)
except (OSError, json.JSONDecodeError) as exc:
    print(f"[start_basalt][ERROR] invalid {label} JSON: {path}: {exc}", file=sys.stderr)
    raise SystemExit(1)

if not isinstance(document, dict):
    print(
        f"[start_basalt][ERROR] invalid {label} JSON: top-level value must be an object: {path}",
        file=sys.stderr,
    )
    raise SystemExit(1)

if not isinstance(document.get("value0"), dict):
    print(
        f"[start_basalt][ERROR] invalid {label} JSON: 'value0' must be an object: {path}",
        file=sys.stderr,
    )
    raise SystemExit(1)
PY_JSON_CHECK
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

TRANSPORT="${BASALT_TRANSPORT:-${TRANSPORT:-shm}}"
CAPTURE_REGISTRY="${CAPTURE_REGISTRY:-/tmp/capture_service_streams.json}"
CAPTURE_TCP_HOST="${CAPTURE_TCP_HOST:-127.0.0.1}"
CAPTURE_TCP_PORT="${CAPTURE_TCP_PORT:-45660}"
CAM0_STREAM="${CAM0_STREAM:-camera0}"
CAM1_STREAM="${CAM1_STREAM:-camera1}"
IMU_STREAM="${IMU_STREAM:-imu0}"

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

# Explicit Basalt estimator mode. Consume these launcher arguments before
# forwarding unrelated options through capture_profile.sh and into the backend.
#
# Supported forms:
#   --mode vio | --mode vo
#   --mode=vio | --mode=vo
#   --vio | --vo
#   --no-imu                 Compatibility alias for --mode vo
BASALT_MODE="${BASALT_MODE:-vio}"
BASALT_MODE_FORWARD_ARGS=()
while [[ $# -gt 0 ]]; do
  case "$1" in
    --mode)
      [[ $# -ge 2 ]] || {
        echo "[start_basalt][ERROR] --mode requires 'vio' or 'vo'" >&2
        exit 2
      }
      BASALT_MODE="$2"
      shift 2
      ;;
    --mode=*)
      BASALT_MODE="${1#--mode=}"
      shift
      ;;
    --vio)
      BASALT_MODE="vio"
      shift
      ;;
    --vo|--no-imu)
      BASALT_MODE="vo"
      shift
      ;;
    --)
      shift
      BASALT_MODE_FORWARD_ARGS+=("$@")
      break
      ;;
    *)
      BASALT_MODE_FORWARD_ARGS+=("$1")
      shift
      ;;
  esac
done
set -- "${BASALT_MODE_FORWARD_ARGS[@]}"

BASALT_MODE="$(printf '%s' "$BASALT_MODE" | tr '[:upper:]' '[:lower:]')"
case "$BASALT_MODE" in
  vio|vo) ;;
  *)
    echo "[start_basalt][ERROR] BASALT_MODE must be 'vio' or 'vo', got: $BASALT_MODE" >&2
    exit 2
    ;;
esac

capture_profile_parse_cli "$@"
set -- "${CAPTURE_PROFILE_FORWARD_ARGS[@]}"
capture_profile_load_backend \
  "basalt_vio" \
  "${CAPTURE_PROFILE_CLI_OVERRIDE:-${BASALT_PROFILE:-${CAPTURE_PROFILE:-}}}" \
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
BASALT_CALIB="${BASALT_CALIB:-$FINAL_PROFILE_DIR/basalt_calib_${CALIB_PROFILE_NAME}.json}"
BASALT_VIO_CONFIG="${BASALT_VIO_CONFIG:-$FINAL_PROFILE_DIR/basalt_vio_config_${CALIB_PROFILE_NAME}.json}"
BASALT_VO_CONFIG="${BASALT_VO_CONFIG:-$FINAL_PROFILE_DIR/basalt_vo_config_${CALIB_PROFILE_NAME}.json}"
BASALT_CALIB="$(expand_tilde "$BASALT_CALIB")"
BASALT_VIO_CONFIG="$(expand_tilde "$BASALT_VIO_CONFIG")"
BASALT_VO_CONFIG="$(expand_tilde "$BASALT_VO_CONFIG")"

BASALT_CONFIG="$BASALT_VIO_CONFIG"
BASALT_ESTIMATOR_ARGS=()
if [[ "$BASALT_MODE" == "vo" ]]; then
  BASALT_CONFIG="$BASALT_VO_CONFIG"
  BASALT_ESTIMATOR_ARGS+=(--no-imu)
fi

validate_basalt_json_object "camera calibration" "$BASALT_CALIB"
validate_basalt_json_object "${BASALT_MODE^^} config" "$BASALT_CONFIG"

OUT_DIR="${OUT_DIR:-/tmp/xr_basalt_unified_live}"
XR_BACKEND_CONTROL_FILE="${XR_BACKEND_CONTROL_FILE:-/tmp/xr_backend_control.json}"
export XR_BACKEND_CONTROL_FILE

STARTUP_GATE="${STARTUP_GATE:-0}"
STARTUP_GATE_SCRIPT="${STARTUP_GATE_SCRIPT:-$ROOT_PROJECT/tools/xr_startup_gate.py}"
STARTUP_GATE_SCRIPT="$(expand_tilde "$STARTUP_GATE_SCRIPT")"
STARTUP_GATE_TIMEOUT_SEC="${STARTUP_GATE_TIMEOUT_SEC:-0}"
STARTUP_GATE_PRINT_EVERY="${STARTUP_GATE_PRINT_EVERY:-5}"
STARTUP_GATE_VISUAL="${STARTUP_GATE_VISUAL:-1}"
STARTUP_GATE_IMU_DEFAULT=1
if [[ "$BASALT_MODE" == "vo" ]]; then
  STARTUP_GATE_IMU_DEFAULT=0
fi
STARTUP_GATE_IMU="${STARTUP_GATE_IMU:-$STARTUP_GATE_IMU_DEFAULT}"
if [[ "$BASALT_MODE" == "vo" && "$STARTUP_GATE_IMU" == "1" ]]; then
  echo "[start_basalt][WARN] disabling STARTUP_GATE_IMU in VO mode"
  STARTUP_GATE_IMU=0
fi
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
echo "[start_basalt] CAPTURE_PROFILE_SOURCE=$CAPTURE_PROFILE_SOURCE_RESOLVED"
echo "[start_basalt] TRANSPORT=$TRANSPORT"
echo "[start_basalt] PROFILE_FILE=$CAPTURE_PROFILE_FILE_RESOLVED"
echo "[start_basalt] BASALT_BIN_DIR=$BASALT_BIN_DIR"
echo "[start_basalt] BASALT_LIB_DIR=$BASALT_LIB_DIR"
echo "[start_basalt] FINAL_PROFILE_DIR=$FINAL_PROFILE_DIR"
echo "[start_basalt] BASALT_MODE=$BASALT_MODE"
echo "[start_basalt] BASALT_CALIB=$BASALT_CALIB"
echo "[start_basalt] BASALT_VIO_CONFIG=$BASALT_VIO_CONFIG"
echo "[start_basalt] BASALT_VO_CONFIG=$BASALT_VO_CONFIG"
echo "[start_basalt] BASALT_CONFIG=$BASALT_CONFIG"
echo "[start_basalt] STARTUP_GATE_IMU=$STARTUP_GATE_IMU"
echo "[start_basalt] XR_BACKEND_CONTROL_FILE=$XR_BACKEND_CONTROL_FILE"
echo "[start_basalt] STARTUP_GATE=$STARTUP_GATE"
echo "[start_basalt] STARTUP_GATE_SCRIPT=$STARTUP_GATE_SCRIPT"
echo "[start_basalt] LD_LIBRARY_PATH=${LD_LIBRARY_PATH:-}"

if [[ "$STARTUP_GATE" == "1" ]]; then
  if [[ "$TRANSPORT" != "shm" ]]; then
    echo "[start_basalt][ERROR] STARTUP_GATE currently supports only TRANSPORT=shm" >&2
    exit 2
  fi
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

backend_args=(
  --transport "$TRANSPORT"
  --registry "$CAPTURE_REGISTRY"
  --tcp-host "$CAPTURE_TCP_HOST"
  --tcp-port "$CAPTURE_TCP_PORT"
  --cam0-stream "$CAM0_STREAM"
  --cam1-stream "$CAM1_STREAM"
  --imu-stream "$IMU_STREAM"
  --cam-calib "$BASALT_CALIB"
  --config-path "$BASALT_CONFIG"
  --out-dir "$OUT_DIR"
  --duration 0
  --image-scale 256
  --no-enforce-realtime
)
backend_args+=("${BASALT_ESTIMATOR_ARGS[@]}")
backend_args+=("$@")

exec "$BASALT_BIN_DIR/capture_basalt_backend" "${backend_args[@]}"
