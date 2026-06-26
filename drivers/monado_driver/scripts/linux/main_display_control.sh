#!/usr/bin/env bash
set -euo pipefail

# Helper for toggling/restoring the main desktop monitor when using XREAL + Monado XCB fullscreen.
# Defaults match the current XREAL output seen on this machine.

XR_OUT="${XR_TRACKING_MONADO_XCB_OUTPUT:-${XR_OUT:-DisplayPort-1-1}}"
XR_MODE="${XR_TRACKING_MONADO_XCB_MODE:-${XR_MODE:-3840x1080}}"
XR_RATE="${XR_TRACKING_MONADO_XCB_RATE_HZ:-${XR_RATE:-60}}"
MAIN_OUT="${XR_TRACKING_MAIN_OUTPUT:-${MAIN_OUT:-}}"
LAYOUT="${XR_TRACKING_DISPLAY_LAYOUT:-main-left}" # main-left or xr-left

usage() {
  cat <<USAGE
Usage:
  $0 status
  $0 off-main
  $0 on-main
  $0 exclusive-xr
  $0 restore

Environment:
  XR_TRACKING_MONADO_XCB_OUTPUT      XREAL output, default: DisplayPort-1-1
  XR_TRACKING_MONADO_XCB_MODE        XREAL mode, default: 3840x1080
  XR_TRACKING_MONADO_XCB_RATE_HZ     XREAL refresh rate, default: 60
  XR_TRACKING_MAIN_OUTPUT            Main monitor output. If empty, auto-detected as first connected output != XREAL.
  XR_TRACKING_DISPLAY_LAYOUT         main-left or xr-left, default: main-left

Examples:
  XR_TRACKING_MAIN_OUTPUT=DP-6 $0 off-main
  XR_TRACKING_MAIN_OUTPUT=DP-6 $0 on-main
  XR_TRACKING_MONADO_XCB_OUTPUT=DisplayPort-1-1 XR_TRACKING_MONADO_XCB_RATE_HZ=90 $0 exclusive-xr
USAGE
}

connected_outputs() {
  xrandr --query | awk '/ connected/ {print $1}'
}

auto_main_out() {
  if [[ -n "${MAIN_OUT}" ]]; then
    printf '%s\n' "${MAIN_OUT}"
    return 0
  fi
  xrandr --query | awk -v xr="${XR_OUT}" '/ connected/ && $1 != xr {print $1; exit}'
}

require_output() {
  local out="$1"
  local label="$2"
  if [[ -z "${out}" ]]; then
    echo "[main_display_control][ERROR] ${label} output is empty" >&2
    return 1
  fi
  if ! xrandr --query | awk '/ connected/ {print $1}' | grep -qx -- "${out}"; then
    echo "[main_display_control][ERROR] ${label} output is not connected: ${out}" >&2
    echo "[main_display_control] connected outputs:" >&2
    xrandr --query | grep ' connected' >&2 || true
    return 1
  fi
}

set_xr_mode() {
  require_output "${XR_OUT}" "XREAL"
  xrandr --output "${XR_OUT}" --set non-desktop 0 || true
  xrandr --output "${XR_OUT}" --mode "${XR_MODE}" --rate "${XR_RATE}"
}

status() {
  echo "[main_display_control] XR_OUT=${XR_OUT} XR_MODE=${XR_MODE} XR_RATE=${XR_RATE} MAIN_OUT=${MAIN_OUT:-auto} LAYOUT=${LAYOUT}"
  xrandr --query | grep ' connected' || true
  echo
  xrandr --query | awk -v xr="${XR_OUT}" '
    $1 == xr {print; show=1; next}
    show && /^[[:space:]]/ {print; next}
    show {show=0}
  '
}

off_main() {
  local main
  main="$(auto_main_out)"
  require_output "${main}" "main monitor"
  echo "[main_display_control] turning off main monitor: ${main}"
  xrandr --output "${main}" --off
}

on_main() {
  local main
  main="$(auto_main_out)"
  require_output "${main}" "main monitor"
  require_output "${XR_OUT}" "XREAL"

  echo "[main_display_control] enabling main monitor: ${main}"
  echo "[main_display_control] keeping XREAL output: ${XR_OUT} ${XR_MODE}@${XR_RATE}"

  if [[ "${LAYOUT}" == "xr-left" ]]; then
    xrandr --output "${XR_OUT}" --set non-desktop 0 || true
    xrandr --output "${XR_OUT}" --mode "${XR_MODE}" --rate "${XR_RATE}" --primary --pos 0x0
    xrandr --output "${main}" --auto --right-of "${XR_OUT}"
  else
    xrandr --output "${main}" --auto --primary --pos 0x0
    xrandr --output "${XR_OUT}" --set non-desktop 0 || true
    xrandr --output "${XR_OUT}" --mode "${XR_MODE}" --rate "${XR_RATE}" --right-of "${main}"
  fi
}

exclusive_xr() {
  local main
  main="$(auto_main_out || true)"
  echo "[main_display_control] switching to exclusive XREAL framebuffer"
  echo "[main_display_control] XR_OUT=${XR_OUT} XR_MODE=${XR_MODE} XR_RATE=${XR_RATE} MAIN_OUT=${main:-none}"

  if [[ -n "${main}" ]]; then
    xrandr --output "${main}" --off || true
  fi
  require_output "${XR_OUT}" "XREAL"
  xrandr --output "${XR_OUT}" --set non-desktop 0 || true
  xrandr --output "${XR_OUT}" --mode "${XR_MODE}" --rate "${XR_RATE}" --primary --pos 0x0 --fb "${XR_MODE}"
}

cmd="${1:-}"
case "${cmd}" in
  status) status ;;
  off-main|off) off_main ;;
  on-main|on|restore) on_main ;;
  exclusive-xr|xr-only) exclusive_xr ;;
  -h|--help|help|'') usage ;;
  *)
    echo "[main_display_control][ERROR] unknown command: ${cmd}" >&2
    usage >&2
    exit 2
    ;;
esac
