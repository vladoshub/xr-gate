#!/usr/bin/env bash
set -euo pipefail

log() { echo "[restore_xreal_desktop] $*" >&2; }

XR_OUT="${XR_STEAMVR_RESTORE_OUTPUT:-${XR_OPENVR_DGPU_OUTPUT:-${XR_OUT:-DP-4}}}"
MAIN_OUT="${XR_STEAMVR_RESTORE_MAIN_OUTPUT:-${MAIN_OUT:-DP-6}}"
XR_MODE="${XR_STEAMVR_RESTORE_MODE:-}"
XR_RATE="${XR_STEAMVR_RESTORE_RATE_HZ:-60}"
LAYOUT="${XR_STEAMVR_RESTORE_LAYOUT:-right-of}"
STOP_STEAMVR="${XR_STEAMVR_RESTORE_STOP_STEAMVR:-1}"
TRY_2D="${XR_STEAMVR_RESTORE_TO_2D:-1}"

if [[ "$STOP_STEAMVR" == "1" ]]; then
  pkill -f vrserver || true
  pkill -f vrcompositor || true
  pkill -f vrmonitor || true
  pkill -f vrdashboard || true
  pkill -f vrwebhelper || true
fi

pkill -f ar-drivers-rs || true
pkill -f xreal_display_helper || true
sleep 1

if [[ "$TRY_2D" == "1" ]]; then
  if command -v ar-drivers-rs >/dev/null 2>&1; then
    log "requesting XREAL 2D mode via ar-drivers-rs"
    ar-drivers-rs set_to_2d >/dev/null 2>&1 || true
    sleep 2
  fi
fi

if ! xrandr --query | awk '/ connected| disconnected/ {print $1}' | grep -qx -- "$XR_OUT"; then
  log "output not found: $XR_OUT"
  xrandr --query | grep -E ' connected| disconnected' >&2 || true
  exit 2
fi

if xrandr --query | awk '/ connected| disconnected/ {print $1}' | grep -qx -- "$MAIN_OUT"; then
  log "enabling main output: $MAIN_OUT"
  xrandr --output "$MAIN_OUT" --auto --primary --pos 0x0 || true
else
  log "main output not found, skipping: $MAIN_OUT"
fi

log "restoring XREAL desktop output: output=$XR_OUT layout=$LAYOUT mode=${XR_MODE:-auto} rate=${XR_RATE}Hz"
xrandr --output "$XR_OUT" --set non-desktop 0 || true

case "$LAYOUT" in
  off)
    xrandr --output "$XR_OUT" --off || true
    ;;
  auto)
    xrandr --output "$XR_OUT" --auto || true
    ;;
  same-as|same_as|mirror)
    if [[ -n "$XR_MODE" ]]; then
      xrandr --output "$XR_OUT" --mode "$XR_MODE" --rate "$XR_RATE" --same-as "$MAIN_OUT" || \
        xrandr --output "$XR_OUT" --auto --same-as "$MAIN_OUT" || true
    else
      xrandr --output "$XR_OUT" --auto --same-as "$MAIN_OUT" || true
    fi
    ;;
  right-of|right_of)
    if [[ -n "$XR_MODE" ]]; then
      xrandr --output "$XR_OUT" --mode "$XR_MODE" --rate "$XR_RATE" --right-of "$MAIN_OUT" || \
        xrandr --output "$XR_OUT" --auto --right-of "$MAIN_OUT" || true
    else
      xrandr --output "$XR_OUT" --auto --right-of "$MAIN_OUT" || true
    fi
    ;;
  left-of|left_of)
    if [[ -n "$XR_MODE" ]]; then
      xrandr --output "$XR_OUT" --mode "$XR_MODE" --rate "$XR_RATE" --left-of "$MAIN_OUT" || \
        xrandr --output "$XR_OUT" --auto --left-of "$MAIN_OUT" || true
    else
      xrandr --output "$XR_OUT" --auto --left-of "$MAIN_OUT" || true
    fi
    ;;
  *)
    log "unsupported XR_STEAMVR_RESTORE_LAYOUT=$LAYOUT; expected right-of, left-of, same-as, auto, or off"
    exit 2
    ;;
esac

xrandr --query --prop | sed -n "/^$XR_OUT /,/^[^[:space:]].* connected/p" || true
