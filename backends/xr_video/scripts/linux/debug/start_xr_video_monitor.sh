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
XR_VIDEO_MONITOR_BIN="${XR_VIDEO_MONITOR_BIN:-$BIN_DIR/xr_video_monitor}"

VIDEO_INPUT="${VIDEO_INPUT:-tcp}"
VIDEO_TCP_HOST="${VIDEO_TCP_HOST:-127.0.0.1}"
VIDEO_TCP_PORT="${VIDEO_TCP_PORT:-45700}"
VIDEO_REGISTRY="${VIDEO_REGISTRY:-/tmp/xr_video_streams.json}"
VIDEO_STREAM="${VIDEO_STREAM:-stereo_video}"
DURATION="${DURATION:-10}"
PRINT_EVERY="${PRINT_EVERY:-30}"
POLL_TIMEOUT="${POLL_TIMEOUT:-1.0}"

ROOT_PROJECT="$(expand_tilde "$ROOT_PROJECT")"
BIN_DIR="$(expand_tilde "$BIN_DIR")"
XR_VIDEO_MONITOR_BIN="$(expand_tilde "$XR_VIDEO_MONITOR_BIN")"

if [[ ! -x "$XR_VIDEO_MONITOR_BIN" ]]; then
  echo "[ERROR] xr_video_monitor binary not found or not executable: $XR_VIDEO_MONITOR_BIN" >&2
  echo "[ERROR] Build it first:" >&2
  echo "  $ROOT_PROJECT/backends/xr_video/scripts/linux/install_xr_video.sh" >&2
  exit 1
fi

exec "$XR_VIDEO_MONITOR_BIN" \
  --input "$VIDEO_INPUT" \
  --tcp-host "$VIDEO_TCP_HOST" \
  --tcp-port "$VIDEO_TCP_PORT" \
  --video-registry "$VIDEO_REGISTRY" \
  --video-stream "$VIDEO_STREAM" \
  --duration "$DURATION" \
  --print-every "$PRINT_EVERY" \
  --poll-timeout "$POLL_TIMEOUT" \
  "$@"
