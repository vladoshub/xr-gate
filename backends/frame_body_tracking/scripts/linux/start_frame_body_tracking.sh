#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_PROJECT="${ROOT_PROJECT:-$(cd "$SCRIPT_DIR/../../../.." && pwd)}"
bin_root="${XR_BIN_ROOT:-$ROOT_PROJECT/bin}"
backend="$bin_root/backends/frame_body_tracking/frame_body_tracking_backend"

if [[ ! -x "$backend" ]]; then
  echo "[frame_body_tracking] backend binary missing: $backend" >&2
  exit 1
fi

capture_registry="${XR_FRAME_CAPTURE_REGISTRY:-${CAPTURE_REGISTRY:-${XR_RUNTIME_DIR:-${TMPDIR:-/tmp}}/capture_service_streams.json}}"
args=(
  --capture-registry "${capture_registry/#\~/${HOME:-~}}"
  --tracking-registry "${TRACKING_REGISTRY:-${XR_RUNTIME_DIR:-${TMPDIR:-/tmp}}/tracking_streams.json}"
  --pose-stream "${HMD_STREAM:-hmd_pose}"
  --output-stream "${BODY_TRACKERS_STREAM:-body_trackers}"
  --ort-threads "${XR_FRAME_THREADS:-4}"
  --pitch-down-deg "${XR_FRAME_PITCH_DOWN_DEG:-0}"
  --max-camera-pitch-deg "${XR_FRAME_MAX_CAMERA_PITCH_DEG:-0}"
  --max-fps "${XR_FRAME_MAX_FPS:-20}"
  --max-stereo-disagreement-m "${XR_FRAME_MAX_STEREO_DISAGREEMENT_M:-0.24}"
)

cam0="${XR_FRAME_CAM0_STREAM:-}"
cam1="${XR_FRAME_CAM1_STREAM:-}"
if [[ -z "${XR_FRAME_CAPTURE_REGISTRY:-}" && -z "${XR_FRAME_STEREO_PAIR:-}" ]]; then
  cam0="${cam0:-${CAM0_STREAM:-}}"
  cam1="${cam1:-${CAM1_STREAM:-}}"
fi
if [[ -n "${XR_FRAME_STEREO_PAIR:-}" ]]; then
  args+=(--stereo-pair "$XR_FRAME_STEREO_PAIR")
fi
if [[ -n "$cam0" ]]; then args+=(--cam0-stream "$cam0"); fi
if [[ -n "$cam1" ]]; then args+=(--cam1-stream "$cam1"); fi

model="${XR_FRAME_MODEL:-$bin_root/body-tracking-models/frame/frame_backbone.onnx}"
calib="${XR_FRAME_CALIB:-${BASALT_CALIB:-}}"
if [[ -z "$calib" && -n "${FINAL_PROFILE_DIR:-}" && -n "${CALIB_PROFILE_NAME:-}" ]]; then
  calib="$FINAL_PROFILE_DIR/basalt_calib_${CALIB_PROFILE_NAME}.json"
fi
args+=(--model "${model/#\~/${HOME:-~}}")
if [[ -n "$calib" ]]; then
  args+=(--calib "${calib/#\~/${HOME:-~}}")
fi
basalt_mode="${BASALT_MODE:-vio}"
if [[ "${basalt_mode,,}" == vo ]]; then
  args+=(--camera-time-offset-ns 0)
fi

exec "$backend" "${args[@]}" "$@"
