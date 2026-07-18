#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=load_target.sh
source "$SCRIPT_DIR/load_target.sh"

RECORD_MODE="${RECORD_MODE:-${DEFAULT_RECORD_MODE:-stereo_imu}}"
case "$RECORD_MODE" in
  camera_only)
    NO_IMU=1
    ACTIVE_CAPTURE_CONFIG="$CAPTURE_CONFIG_CAMERA_ONLY"
    ;;
  stereo_imu)
    NO_IMU=0
    ACTIVE_CAPTURE_CONFIG="$CAPTURE_CONFIG_STEREO_IMU"
    ;;
  *)
    echo "[record][ERROR] RECORD_MODE must be camera_only or stereo_imu, got: $RECORD_MODE" >&2
    exit 1
    ;;
esac

if [[ -x "$PACKAGE_ROOT/bin/python-runtime/venv/bin/python" ]]; then
  PYTHON_BIN="${PYTHON_BIN:-$PACKAGE_ROOT/bin/python-runtime/venv/bin/python}"
elif [[ -x "${VENV_DIR:-$HOME/.venvs/xreal_capture_service}/bin/python" ]]; then
  PYTHON_BIN="${PYTHON_BIN:-${VENV_DIR:-$HOME/.venvs/xreal_capture_service}/bin/python}"
else
  PYTHON_BIN="${PYTHON_BIN:-python3}"
fi

export XR_PACKAGE_ROOT="$PACKAGE_ROOT"
export PYTHONPATH="$PACKAGE_ROOT/bin/python:$PACKAGE_ROOT/bin/python/capture_service:$ROOT_PROJECT:$ROOT_PROJECT/capture_service:${PYTHONPATH:-}"
export PYTHONUNBUFFERED="${PYTHONUNBUFFERED:-1}"

SECONDS_TOTAL="${SECONDS_TOTAL:-90}"
TRANSPORT="${TRANSPORT:-shm}"
REGISTRY="${REGISTRY:-/tmp/capture_service_streams.json}"
TCP_HOST="${TCP_HOST:-127.0.0.1}"
TCP_PORT="${TCP_PORT:-45660}"
CAM0_STREAM="${CAM0_STREAM:-camera0}"
CAM1_STREAM="${CAM1_STREAM:-camera1}"
IMU_STREAM="${IMU_STREAM:-imu0}"
START_CAPTURE_SERVICE="${START_CAPTURE_SERVICE:-1}"
STOP_CAPTURE_SERVICE="${STOP_CAPTURE_SERVICE:-$START_CAPTURE_SERVICE}"
PUBLISH="${PUBLISH:-shm}"
WARMUP_SECONDS="${WARMUP_SECONDS:-2.0}"
STEREO_MAX_DELTA_MS="${STEREO_MAX_DELTA_MS:-1.0}"

if [[ ! -f "$RECORDER" ]]; then
  echo "[record][ERROR] recorder not found: $RECORDER" >&2
  exit 1
fi
if [[ "$START_CAPTURE_SERVICE" == "1" ]]; then
  [[ -x "$CAPTURE_START_SCRIPT" ]] || { echo "[record][ERROR] launcher not executable: $CAPTURE_START_SCRIPT" >&2; exit 1; }
  [[ -f "$ACTIVE_CAPTURE_CONFIG" ]] || { echo "[record][ERROR] capture config not found: $ACTIVE_CAPTURE_CONFIG" >&2; exit 1; }
fi

print_target_summary
echo "RECORD_MODE=$RECORD_MODE"
echo "ACTIVE_CAPTURE_CONFIG=$ACTIVE_CAPTURE_CONFIG"
echo "RECORDER=$RECORDER"
echo "PYTHON_BIN=$PYTHON_BIN"
echo

echo "[guided] Before start:"
echo "  1. Fix the AprilGrid so it cannot move."
echo "  2. Move the complete camera assembly; do not flex the camera/IMU mount."
echo "  3. Keep the grid visible in both camera streams."
echo "  4. Use smooth rotations and translations without sudden jerks."
if [[ "$NO_IMU" == "1" ]]; then
  echo "  5. Camera-only mode: this dataset calibrates stereo cameras only."
else
  echo "  5. Stereo+IMU mode: keep the camera and IMU rigidly attached."
fi
read -rp "[guided] Press Enter to start ${SECONDS_TOTAL}s recording..."

args=(
  "$RECORDER"
  --target-name "$CALIB_TARGET_NAME"
  --transport "$TRANSPORT"
  --registry "$REGISTRY"
  --tcp-host "$TCP_HOST"
  --tcp-port "$TCP_PORT"
  --cam0-stream "$CAM0_STREAM"
  --cam1-stream "$CAM1_STREAM"
  --imu-stream "$IMU_STREAM"
  --seconds "$SECONDS_TOTAL"
  --out-root "$RECORDS_DIR"
  --name-prefix "$RECORD_PREFIX"
  --expect-width "$EXPECT_WIDTH"
  --expect-height "$EXPECT_HEIGHT"
  --warmup-seconds "$WARMUP_SECONDS"
  --stereo-max-delta-ms "$STEREO_MAX_DELTA_MS"
)

if [[ "$NO_IMU" == "1" ]]; then
  args+=(--no-imu)
fi
if [[ "$START_CAPTURE_SERVICE" == "1" ]]; then
  args+=(
    --start-capture-service
    --package-root "$PACKAGE_ROOT"
    --capture-start-script "$CAPTURE_START_SCRIPT"
    --capture-config "$ACTIVE_CAPTURE_CONFIG"
    --publish "$PUBLISH"
    --capture-service-log "/tmp/${CALIB_TARGET_NAME}_calibration_capture_service.log"
  )
  if [[ "$STOP_CAPTURE_SERVICE" == "1" ]]; then
    args+=(--stop-capture-service)
  fi
fi

OUT_LOG="/tmp/${CALIB_TARGET_NAME}_guided_record.log"
rm -f "$OUT_LOG"
"$PYTHON_BIN" "${args[@]}" > >(tee "$OUT_LOG") 2>&1 &
REC_PID=$!
GUIDE_START="$(date +%s)"

cleanup() {
  if kill -0 "$REC_PID" 2>/dev/null; then
    echo
    echo "[guided] stopping recorder pid=$REC_PID"
    kill "$REC_PID" 2>/dev/null || true
  fi
}
trap cleanup INT TERM

say_at() {
  local target_s="$1"
  local msg="$2"
  while true; do
    kill -0 "$REC_PID" 2>/dev/null || return 1
    local elapsed=$(( $(date +%s) - GUIDE_START ))
    local remain=$(( target_s - elapsed ))
    (( remain <= 0 )) && break
    sleep 1
  done
  printf '\a'
  echo
  echo "================================================================"
  echo "[T+${target_s}s] $msg"
  echo "================================================================"
  command -v notify-send >/dev/null 2>&1 && notify-send "$CALIB_LABEL calibration" "$msg" || true
}

say_at 0  "Center the AprilGrid and hold almost still for 2-3 seconds."
say_at 5  "Yaw: slowly rotate left and right while keeping the grid visible."
say_at 15 "Pitch: slowly tilt up and down."
say_at 25 "Roll: rotate around the optical axis."
say_at 35 "Translation X/Y: move left/right and up/down."
say_at 50 "Translation Z: move closer to and farther from the grid."
say_at 65 "Combined arcs: mix rotation and translation across the image."
say_at 80 "Return the grid toward the center with small smooth motions."
say_at 88 "Almost done: keep the target visible in both cameras."

wait "$REC_PID"

echo
echo "[guided] Recorder finished. Last log lines:"
tail -20 "$OUT_LOG" || true
echo
echo "[guided] Latest dataset:"
ls -1dt "$RECORDS_DIR"/$DATASET_GLOB 2>/dev/null | head -n1 || true
