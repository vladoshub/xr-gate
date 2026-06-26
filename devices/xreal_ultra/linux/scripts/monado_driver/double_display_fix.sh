#!/usr/bin/env bash
set -Eeuo pipefail

# Monado/XCB fullscreen helper for XREAL-style SBS displays.
#
# What it does:
#   1. Saves enough of the current xrandr layout to restore the main monitor.
#   2. Temporarily makes the glasses the only active X11 framebuffer.
#   3. Runs Monado in foreground.
#   4. Restores the main monitor after Monado exits or Ctrl+C is pressed.
#
# Typical packaged usage:
#   cd ~/src/xr_tracking/out/xreal_ultra
#   XR_TRACKING_MONADO_COMPOSITOR_MODE=xcb_fullscreen \
#   XR_TRACKING_MONADO_XCB_OUTPUT=DisplayPort-1-1 \
#     devices/xreal_ultra/linux/scripts/monado_driver/double_display_fix.sh
#
# Optional custom command:
#   double_display_fix.sh -- /path/to/start_monado_driver.sh

log() {
  echo "[double_display_fix] $*"
}

fatal() {
  echo "[double_display_fix][ERROR] $*" >&2
  exit 1
}

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Resolve the root from either:
#   <repo>/devices/xreal_ultra/linux/scripts/monado_driver
# or:
#   <repo>/out/xreal_ultra/devices/xreal_ultra/linux/scripts/monado_driver
ROOT_PROJECT="$(cd "$SCRIPT_DIR/../../../../.." && pwd)"
ADJACENT_START_SCRIPT="$SCRIPT_DIR/start_monado_driver.sh"
PACKAGED_START_SCRIPT="$ROOT_PROJECT/out/xreal_ultra/devices/xreal_ultra/linux/scripts/monado_driver/start_monado_driver.sh"

# When this helper is launched from the source tree, prefer the packaged
# out/xreal_ultra wrapper if it exists. The source wrapper may point at
# <repo>/bin/scripts/... and fail when only the device package was installed.
# When this helper is already launched from out/xreal_ultra, the packaged path
# above intentionally does not exist, so the adjacent script is used.
DEFAULT_START_SCRIPT="$ADJACENT_START_SCRIPT"
if [ -x "$PACKAGED_START_SCRIPT" ]; then
  DEFAULT_START_SCRIPT="$PACKAGED_START_SCRIPT"
fi

log "ROOT_PROJECT=$ROOT_PROJECT"
log "DEFAULT_START_SCRIPT=$DEFAULT_START_SCRIPT"

XR_MODE="${XR_TRACKING_MONADO_XCB_MODE:-${XR_MODE:-3840x1080}}"
XR_RATE="${XR_TRACKING_MONADO_XCB_RATE_HZ:-${FREQ:-60}}"
XR_OUT="${XR_TRACKING_MONADO_XCB_OUTPUT:-${XR_OUT:-}}"
RESTORE_LAYOUT="${XR_TRACKING_MONADO_XCB_RESTORE_LAYOUT:-1}"
MAKE_PRIMARY="${XR_TRACKING_MONADO_XCB_MAKE_PRIMARY:-1}"

if ! command -v xrandr >/dev/null 2>&1; then
  fatal "xrandr not found"
fi

if [ -z "$XR_OUT" ]; then
  XR_OUT="$(xrandr --query | awk -v mode="$XR_MODE" '
    / connected/ {out=$1}
    $1 == mode {print out; exit}
  ')"
fi

if [ -z "$XR_OUT" ]; then
  fatal "could not auto-detect XREAL output with mode $XR_MODE; set XR_TRACKING_MONADO_XCB_OUTPUT=..."
fi

if ! xrandr --query | awk -v out="$XR_OUT" '$1 == out && / connected/ {found=1} END {exit found ? 0 : 1}'; then
  xrandr --query | grep " connected" >&2 || true
  fatal "XR output '$XR_OUT' is not connected"
fi

PRIMARY_OUT="$(xrandr --query | awk '/ connected primary/ {print $1; exit}')"
MAIN_OUT="${XR_TRACKING_MONADO_MAIN_OUTPUT:-}"
if [ -z "$MAIN_OUT" ]; then
  if [ -n "$PRIMARY_OUT" ] && [ "$PRIMARY_OUT" != "$XR_OUT" ]; then
    MAIN_OUT="$PRIMARY_OUT"
  else
    MAIN_OUT="$(xrandr --query | awk -v xr="$XR_OUT" '/ connected/ && $1 != xr {print $1; exit}')"
  fi
fi

CONNECTED_OUTPUTS="$(xrandr --query | awk '/ connected/ {print $1}')"

log "XR_OUT=$XR_OUT"
log "XR_MODE=$XR_MODE"
log "XR_RATE=$XR_RATE"
log "MAIN_OUT=${MAIN_OUT:-<none>}"
log "RESTORE_LAYOUT=$RESTORE_LAYOUT"

restore_layout() {
  local rc=$?

  if [ "$RESTORE_LAYOUT" != "1" ]; then
    exit "$rc"
  fi

  log "restoring monitor layout"

  if [ -n "$MAIN_OUT" ]; then
    xrandr --output "$MAIN_OUT" --auto --primary --pos 0x0 || true
    xrandr --output "$XR_OUT" --mode "$XR_MODE" --right-of "$MAIN_OUT" || true
  else
    xrandr --output "$XR_OUT" --mode "$XR_MODE" --primary --pos 0x0 || true
  fi

  # Bring other connected outputs back in a conservative way. This keeps the
  # chosen main output primary and places extra outputs to the right.
  for out in $CONNECTED_OUTPUTS; do
    if [ "$out" != "$XR_OUT" ] && [ "$out" != "$MAIN_OUT" ]; then
      xrandr --output "$out" --auto --right-of "${MAIN_OUT:-$XR_OUT}" || true
    fi
  done

  exit "$rc"
}
trap restore_layout EXIT INT TERM

log "switching to exclusive XREAL framebuffer for Monado xcb_fullscreen"

for out in $CONNECTED_OUTPUTS; do
  if [ "$out" != "$XR_OUT" ]; then
    log "turn off $out"
    xrandr --output "$out" --off || true
  fi
done

xrandr --output "$XR_OUT" --set non-desktop 0 || true

if ! xrandr --output "$XR_OUT" --mode "$XR_MODE" --rate "$XR_RATE" --primary --pos 0x0 --fb "$XR_MODE"; then
  log "failed to set rate $XR_RATE for $XR_MODE; retrying without explicit rate"
  xrandr --output "$XR_OUT" --mode "$XR_MODE" --primary --pos 0x0 --fb "$XR_MODE"
fi

if [ "$MAKE_PRIMARY" = "1" ]; then
  xrandr --output "$XR_OUT" --primary || true
fi

export XR_TRACKING_MONADO_COMPOSITOR_MODE="${XR_TRACKING_MONADO_COMPOSITOR_MODE:-xcb_fullscreen}"

if [ "${1:-}" = "--" ]; then
  shift
fi

if [ "$#" -gt 0 ]; then
  CMD=("$@")
else
  if [ ! -x "$DEFAULT_START_SCRIPT" ]; then
    fatal "default start script not executable: $DEFAULT_START_SCRIPT"
  fi
  CMD=("$DEFAULT_START_SCRIPT")
fi

log "running: ${CMD[*]}"

# XR_TRACKING_MONADO_XCB_OUTPUT is consumed by this wrapper to select and
# prepare the exclusive XREAL xrandr output. Do not forward it to
# start_monado_driver.sh in xcb_fullscreen mode: that script uses the same env
# for windowed xcb placement and intentionally rejects it unless
# XR_TRACKING_MONADO_COMPOSITOR_MODE=xcb.
if [ "${XR_TRACKING_MONADO_COMPOSITOR_MODE:-xcb_fullscreen}" != "xcb" ]; then
  env -u XR_TRACKING_MONADO_XCB_OUTPUT "${CMD[@]}"
else
  "${CMD[@]}"
fi
