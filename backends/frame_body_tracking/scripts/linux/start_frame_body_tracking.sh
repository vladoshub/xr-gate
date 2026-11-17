#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_PROJECT="${ROOT_PROJECT:-$(cd "$SCRIPT_DIR/../../../.." && pwd)}"
backend="${XR_BIN_ROOT:-$ROOT_PROJECT/bin}/backends/frame_body_tracking/frame_body_tracking_backend"

if [[ ! -x "$backend" ]]; then
  echo "[frame_body_tracking] backend binary missing: $backend" >&2
  exit 1
fi

args=(
  --capture-registry "${CAPTURE_REGISTRY:-${XR_RUNTIME_DIR:-${TMPDIR:-/tmp}}/capture_service_streams.json}"
  --tracking-registry "${TRACKING_REGISTRY:-${XR_RUNTIME_DIR:-${TMPDIR:-/tmp}}/tracking_streams.json}"
  --cam0-stream "${CAM0_STREAM:-camera0}"
  --cam1-stream "${CAM1_STREAM:-camera1}"
  --pose-stream "${HMD_STREAM:-hmd_pose}"
  --output-stream "${BODY_TRACKERS_STREAM:-body_trackers}"
  --ort-threads "${XR_FRAME_THREADS:-4}"
  --pitch-down-deg "${XR_FRAME_PITCH_DOWN_DEG:-20}"
  --max-fps "${XR_FRAME_MAX_FPS:-20}"
  --max-stereo-disagreement-m "${XR_FRAME_MAX_STEREO_DISAGREEMENT_M:-0.24}"
)

model="${XR_FRAME_MODEL:-}"
calib="${XR_FRAME_CALIB:-${BASALT_CALIB:-}}"
if [[ -n "$model" ]]; then args+=(--model "$model"); fi
if [[ -n "$calib" ]]; then args+=(--calib "$calib"); fi
if [[ "${BASALT_MODE:-vio}" == vo ]]; then
  args+=(--camera-time-offset-ns 0)
fi

exec "$backend" "${args[@]}" "$@"
