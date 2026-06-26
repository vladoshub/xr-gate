#!/usr/bin/env bash
set -euo pipefail

expand_tilde() {
  local value="$1"

  case "$value" in
    "~")
      printf '%s\n' "$HOME"
      ;;
    "~/"*)
      printf '%s\n' "$HOME/${value#"~/"}"
      ;;
    *)
      printf '%s\n' "$value"
      ;;
  esac
}

# Project/calibration roots
ROOT_PROJECT="${ROOT_PROJECT:-$HOME/src/xr_tracking}"
XREAL_CALIB_DIR="${XREAL_CALIB_DIR:-$HOME/xreal_calib}"

# Device/profile naming
XREAL_DEVICE_NAME="${XREAL_DEVICE_NAME:-xreal_air2ultra}"
XREAL_SERIAL="${XREAL_SERIAL:-ZBBM5DZFMP}"
CALIB_PROFILE_NAME="${CALIB_PROFILE_NAME:-unified_480_ccw90}"

# Tool location. Default matches the original script location.
CALIBRATION_DIR="${CALIBRATION_DIR:-$ROOT_PROJECT/xreal_ultra/linux/calibrate}"
CONVERTER="${CONVERTER:-$ROOT_PROJECT/calibration/xreal_ultra/linux/kalibr_to_basalt_unified_json.py}"

# Final profile directory and file names
FINAL_ROOT_DIR="${FINAL_ROOT_DIR:-$XREAL_CALIB_DIR/final/$XREAL_DEVICE_NAME/$XREAL_SERIAL}"
FINAL_PROFILE_DIR="${FINAL_PROFILE_DIR:-$FINAL_ROOT_DIR/$CALIB_PROFILE_NAME}"

CAMCHAIN_NAME="${CAMCHAIN_NAME:-camchain-imucam.yaml}"
BASALT_OUTPUT_NAME="${BASALT_OUTPUT_NAME:-basalt_calib_${CALIB_PROFILE_NAME}.json}"
MERCURY_OUTPUT_NAME="${MERCURY_OUTPUT_NAME:-mercury_calib_${CALIB_PROFILE_NAME}.json}"

JQ_BIN="${JQ_BIN:-jq}"

# Allow paths like ROOT_PROJECT="~/src/xr_tracking"
ROOT_PROJECT="$(expand_tilde "$ROOT_PROJECT")"
XREAL_CALIB_DIR="$(expand_tilde "$XREAL_CALIB_DIR")"
CALIBRATION_DIR="$(expand_tilde "$CALIBRATION_DIR")"
CONVERTER="$(expand_tilde "$CONVERTER")"
FINAL_ROOT_DIR="$(expand_tilde "$FINAL_ROOT_DIR")"
FINAL_PROFILE_DIR="$(expand_tilde "$FINAL_PROFILE_DIR")"

CAMCHAIN="${CAMCHAIN:-$FINAL_PROFILE_DIR/$CAMCHAIN_NAME}"
BASALT_OUT="${BASALT_OUT:-$FINAL_PROFILE_DIR/$BASALT_OUTPUT_NAME}"
MERCURY_OUT="${MERCURY_OUT:-$FINAL_PROFILE_DIR/$MERCURY_OUTPUT_NAME}"

CAMCHAIN="$(expand_tilde "$CAMCHAIN")"
BASALT_OUT="$(expand_tilde "$BASALT_OUT")"
MERCURY_OUT="$(expand_tilde "$MERCURY_OUT")"

JQ_QUERY="${JQ_QUERY:-.value0.resolution,
.value0.cam_time_offset_ns,
.value0.intrinsics[0].intrinsics,
.value0.intrinsics[1].intrinsics,
.value0.T_imu_cam[0],
.value0.T_imu_cam[1]}"

echo "ROOT_PROJECT=$ROOT_PROJECT"
echo "XREAL_CALIB_DIR=$XREAL_CALIB_DIR"
echo "XREAL_DEVICE_NAME=$XREAL_DEVICE_NAME"
echo "XREAL_SERIAL=$XREAL_SERIAL"
echo "CALIB_PROFILE_NAME=$CALIB_PROFILE_NAME"
echo "CALIBRATION_DIR=$CALIBRATION_DIR"
echo "CONVERTER=$CONVERTER"
echo "FINAL_PROFILE_DIR=$FINAL_PROFILE_DIR"
echo "CAMCHAIN=$CAMCHAIN"
echo "BASALT_OUT=$BASALT_OUT"
echo "MERCURY_OUT=$MERCURY_OUT"

if [[ ! -f "$CONVERTER" ]]; then
  echo "[ERROR] Converter not found: $CONVERTER" >&2
  echo "[ERROR] Set CALIBRATION_DIR or CONVERTER to the directory/file that contains kalibr_to_basalt_unified_json.py" >&2
  exit 1
fi

if [[ ! -f "$CAMCHAIN" ]]; then
  echo "[ERROR] camchain not found: $CAMCHAIN" >&2
  echo "[ERROR] Run IMU-camera calibration first, or set CAMCHAIN=/path/to/camchain-imucam.yaml" >&2
  exit 1
fi

mkdir -p "$FINAL_PROFILE_DIR"

python3 "$CONVERTER" \
  --camchain "$CAMCHAIN" \
  --out "$BASALT_OUT"

cp "$BASALT_OUT" "$MERCURY_OUT"

echo
echo "[OK] converted calibration"
echo "BASALT_OUT=$BASALT_OUT"
echo "MERCURY_OUT=$MERCURY_OUT"

if command -v "$JQ_BIN" >/dev/null 2>&1; then
  echo
  echo "[check basalt json]"
  "$JQ_BIN" "$JQ_QUERY" "$BASALT_OUT"
else
  echo
  echo "[WARN] jq not found; skipping JSON preview"
fi
