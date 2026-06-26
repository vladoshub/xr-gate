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
BIN_DIR="${BIN_DIR:-$ROOT_PROJECT/bin/backends/xr_video}"
XR_VIDEO_BACKEND_BIN="${XR_VIDEO_BACKEND_BIN:-$BIN_DIR/xr_video_backend}"

#INPUT_TRANSPORT="${INPUT_TRANSPORT:-capture_tcp}"
INPUT_TRANSPORT="${INPUT_TRANSPORT:-shm}"
REGISTRY="${REGISTRY:-/tmp/capture_service_streams.json}"
CAPTURE_TCP_HOST="${CAPTURE_TCP_HOST:-127.0.0.1}"
CAPTURE_TCP_PORT="${CAPTURE_TCP_PORT:-45660}"
CAM0_STREAM="${CAM0_STREAM:-camera0}"
CAM1_STREAM="${CAM1_STREAM:-camera1}"
IMU_STREAM="${IMU_STREAM:-imu0}"

#VIDEO_OUTPUT="${VIDEO_OUTPUT:-tcp}"
VIDEO_OUTPUT="${VIDEO_OUTPUT:-shm}"
VIDEO_REGISTRY="${VIDEO_REGISTRY:-/tmp/xr_video_streams.json}"
VIDEO_STREAM="${VIDEO_STREAM:-stereo_video}"
VIDEO_SHM_NAME="${VIDEO_SHM_NAME:-/xr_stereo_video_v1}"
VIDEO_FRAME="${VIDEO_FRAME:-camera_stereo}"
VIDEO_SLOTS="${VIDEO_SLOTS:-8}"
VIDEO_TCP_BIND="${VIDEO_TCP_BIND:-0.0.0.0}"
VIDEO_TCP_PORT="${VIDEO_TCP_PORT:-45700}"

DURATION="${DURATION:-0}"
PRINT_EVERY="${PRINT_EVERY:-30}"
MAX_STEREO_DELTA_MS="${MAX_STEREO_DELTA_MS:-1.0}"
ROTATE_DEGREES="${ROTATE_DEGREES:-0}"

ROOT_PROJECT="$(expand_tilde "$ROOT_PROJECT")"
BIN_DIR="$(expand_tilde "$BIN_DIR")"
XR_VIDEO_BACKEND_BIN="$(expand_tilde "$XR_VIDEO_BACKEND_BIN")"

if [[ ! -x "$XR_VIDEO_BACKEND_BIN" ]]; then
  echo "[ERROR] xr_video_backend binary not found or not executable: $XR_VIDEO_BACKEND_BIN" >&2
  echo "[ERROR] Build it first:" >&2
  echo "  $ROOT_PROJECT/backends/xr_video/scripts/linux/install_xr_video.sh" >&2
  exit 1
fi

exec "$XR_VIDEO_BACKEND_BIN" \
  --input-transport "$INPUT_TRANSPORT" \
  --registry "$REGISTRY" \
  --cam0-stream "$CAM0_STREAM" \
  --cam1-stream "$CAM1_STREAM" \
  --imu-stream "$IMU_STREAM" \
  --output "$VIDEO_OUTPUT" \
  --video-registry "$VIDEO_REGISTRY" \
  --video-stream "$VIDEO_STREAM" \
  --video-shm-name "$VIDEO_SHM_NAME" \
  --video-frame "$VIDEO_FRAME" \
  --video-slots "$VIDEO_SLOTS" \
  --duration "$DURATION" \
  --print-every "$PRINT_EVERY" \
  --max-stereo-delta-ms "$MAX_STEREO_DELTA_MS" \
  --rotate-degrees "$ROTATE_DEGREES" \
  "$@"
  
  
  #--video-tcp-bind "$VIDEO_TCP_BIND" \
  #--video-tcp-port "$VIDEO_TCP_PORT" \
  #--tcp-host "$CAPTURE_TCP_HOST" \
  #--tcp-port "$CAPTURE_TCP_PORT" \  
