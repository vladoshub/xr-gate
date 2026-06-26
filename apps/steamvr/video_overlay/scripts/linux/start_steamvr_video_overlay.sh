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

BIN_DIR="${BIN_DIR:-$ROOT_PROJECT/out/xrea_ultra/bin/apps/steamvr/video_overlay}"
BIN_DIR="$(expand_tilde "$BIN_DIR")"

STEAMVR_VIDEO_OVERLAY_BIN="${STEAMVR_VIDEO_OVERLAY_BIN:-$BIN_DIR/steamvr_video_overlay}"
STEAMVR_VIDEO_OVERLAY_BIN="$(expand_tilde "$STEAMVR_VIDEO_OVERLAY_BIN")"

VIDEO_REGISTRY="${VIDEO_REGISTRY:-/tmp/runtime_video_streams.json}"
VIDEO_STREAM="${VIDEO_STREAM:-runtime_stereo_video}"
OVERLAY_WIDTH_M="${OVERLAY_WIDTH_M:-1.20}"
OVERLAY_DISTANCE_M="${OVERLAY_DISTANCE_M:-1.00}"
OVERLAY_X_M="${OVERLAY_X_M:-0.00}"
OVERLAY_Y_M="${OVERLAY_Y_M:-0.00}"
OVERLAY_ALPHA="${OVERLAY_ALPHA:-0.70}"
TARGET_FPS="${TARGET_FPS:-30}"
PRINT_EVERY="${PRINT_EVERY:-30}"
OVERLAY_SBS="${OVERLAY_SBS:-1}"
VIDEO_MODE="${VIDEO_MODE:-sbs}"
OVERLAY_SCALE_DIVISOR="${OVERLAY_SCALE_DIVISOR:-1}"
OVERLAY_UPLOAD_ONCE="${OVERLAY_UPLOAD_ONCE:-0}"
OVERLAY_UPLOAD_BACKEND="${OVERLAY_UPLOAD_BACKEND:-opengl}"
OVERLAY_FLIP_V="${OVERLAY_FLIP_V:-1}"

if [[ ! -x "$STEAMVR_VIDEO_OVERLAY_BIN" ]]; then
  echo "[ERROR] steamvr_video_overlay binary not found or not executable: $STEAMVR_VIDEO_OVERLAY_BIN" >&2
  echo "[ERROR] Build it first:" >&2
  echo "  $ROOT_PROJECT/apps/steamvr/video_overlay/scripts/linux/install_steamvr_video_overlay.sh" >&2
  exit 1
fi

args=(
  --video-registry "$VIDEO_REGISTRY"
  --video-stream "$VIDEO_STREAM"
  --width-m "$OVERLAY_WIDTH_M"
  --distance-m "$OVERLAY_DISTANCE_M"
  --x-m "$OVERLAY_X_M"
  --y-m "$OVERLAY_Y_M"
  --alpha "$OVERLAY_ALPHA"
  --target-fps "$TARGET_FPS"
  --upload-backend "$OVERLAY_UPLOAD_BACKEND"
  --video-mode "$VIDEO_MODE"
  --scale-divisor "$OVERLAY_SCALE_DIVISOR"
  --print-every "$PRINT_EVERY"
)

case "${OVERLAY_SBS,,}" in
  0|false|no|off) args+=(--no-sbs-flag) ;;
esac

case "${OVERLAY_UPLOAD_ONCE,,}" in
  1|true|yes|on) args+=(--upload-once) ;;
esac

case "${OVERLAY_FLIP_V,,}" in
  1|true|yes|on) args+=(--flip-v) ;;
  0|false|no|off) args+=(--no-flip-v) ;;
esac

exec "$STEAMVR_VIDEO_OVERLAY_BIN" "${args[@]}" "$@"
