#!/usr/bin/env bash
set -euo pipefail

# Starts the project-owned XR Tracking Monado runtime driver.
# Works both from:
#   drivers/monado_driver/scripts/linux/start.sh
# and from installed copy:
#   bin/drivers/monado_driver/start.sh

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
XR_MONADO_DEVICE="${XR_MONADO_DEVICE:-${XR_MONADO_DRIVER_DEVICE:-${XR_DEVICE_TARGET:-${XR_TARGET_DEVICE:-xreal_ultra}}}}"
case "${XR_MONADO_DEVICE}" in
  xreal_ultra|xreal_air2ultra)
    XR_MONADO_DEVICE="xreal_ultra"
    ;;
  *)
    # Do not fail here: future device packages may still use the generic runtime
    # driver start script before this script knows their exact env filename.
    ;;
esac

expand_path() {
  local path="$1"
  if [[ "$path" == "~" ]]; then
    printf '%s\n' "$HOME"
  elif [[ "$path" == "~/"* ]]; then
    printf '%s/%s\n' "$HOME" "${path#~/}"
  else
    printf '%s\n' "$path"
  fi
}

is_truthy() {
  case "${1:-}" in
    1|true|TRUE|yes|YES|on|ON) return 0 ;;
    *) return 1 ;;
  esac
}

log() { echo "[start_monado_driver] $*"; }
fail() { echo "[start_monado_driver][ERROR] $*" >&2; exit 1; }


get_xrandr_output_geometry() {
  local output="$1"
  local line=""
  line="$(xrandr --query | awk -v out="$output" '$1 == out && $2 == "connected" {print; exit}')"
  if [[ -z "$line" ]]; then
    return 1
  fi

  if [[ "$line" =~ ([0-9]+)x([0-9]+)\+(-?[0-9]+)\+(-?[0-9]+) ]]; then
    printf '%s %s %s %s\n' "${BASH_REMATCH[3]}" "${BASH_REMATCH[4]}" "${BASH_REMATCH[1]}" "${BASH_REMATCH[2]}"
    return 0
  fi

  return 1
}

find_x_window_for_pid() {
  local pid="$1"
  if command -v xdotool >/dev/null 2>&1; then
    xdotool search --pid "$pid" 2>/dev/null | head -n 1 || true
    return 0
  fi

  if command -v wmctrl >/dev/null 2>&1; then
    wmctrl -lp 2>/dev/null | awk -v pid="$pid" '$3 == pid {print $1; exit}' || true
    return 0
  fi

  return 0
}

move_resize_x_window() {
  local window_id="$1"
  local x="$2"
  local y="$3"
  local w="$4"
  local h="$5"

  if command -v wmctrl >/dev/null 2>&1; then
    wmctrl -ir "$window_id" -b remove,fullscreen,maximized_vert,maximized_horz 2>/dev/null || true
  fi

  if command -v xdotool >/dev/null 2>&1; then
    xdotool windowmove "$window_id" "$x" "$y" 2>/dev/null || true
    xdotool windowsize "$window_id" "$w" "$h" 2>/dev/null || true
    return 0
  fi

  if command -v wmctrl >/dev/null 2>&1; then
    wmctrl -ir "$window_id" -e "0,$x,$y,$w,$h" 2>/dev/null || true
    return 0
  fi

  return 1
}

place_xcb_window_loop() {
  local service_pid="$1"
  local x="$2"
  local y="$3"
  local w="$4"
  local h="$5"
  local wait_sec="${XR_TRACKING_MONADO_XCB_WINDOW_WAIT_SEC:-60}"
  local deadline=$((SECONDS + wait_sec))
  local window_id=""

  if ! command -v xdotool >/dev/null 2>&1 && ! command -v wmctrl >/dev/null 2>&1; then
    log "XCB output placement requested, but neither xdotool nor wmctrl is installed"
    log "install one of them, for example: sudo apt install xdotool wmctrl"
    return 0
  fi

  log "waiting up to ${wait_sec}s for Monado XCB window from pid=$service_pid"
  while kill -0 "$service_pid" 2>/dev/null && (( SECONDS < deadline )); do
    window_id="$(find_x_window_for_pid "$service_pid")"
    if [[ -n "$window_id" ]]; then
      log "placing Monado XCB window id=$window_id at ${x},${y} ${w}x${h}"
      move_resize_x_window "$window_id" "$x" "$y" "$w" "$h" || true
      return 0
    fi
    sleep 0.25
  done

  log "Monado XCB window was not found for pid=$service_pid within ${wait_sec}s"
  return 0
}

run_service_with_xcb_output_placement() {
  local output="$1"
  shift

  local geometry=""
  if ! geometry="$(get_xrandr_output_geometry "$output")"; then
    fail "XR_TRACKING_MONADO_XCB_OUTPUT=$output is not an active xrandr output with geometry"
  fi

  read -r x y w h <<<"$geometry"
  log "XR_TRACKING_MONADO_XCB_OUTPUT=$output geometry=${x},${y} ${w}x${h}"

  if is_truthy "${XR_TRACKING_MONADO_XCB_MAKE_PRIMARY:-0}"; then
    log "setting $output as primary xrandr output"
    xrandr --output "$output" --primary || true
  fi

  # Keep monado-service in the foreground. Monado's IPC server monitors stdin
  # with epoll during startup; running it as a background job from this wrapper can
  # give it a non-epollable stdin and fail with `epoll_ctl(stdin) failed`. Start
  # the window-placement helper before exec: after exec, this shell PID becomes
  # monado-service, so the helper can still search windows by the same PID.
  local service_pid="$$"
  place_xcb_window_loop "$service_pid" "$x" "$y" "$w" "$h" &

  exec "$SERVICE_BIN" "$@"
}

# Walk upwards and find the real project root.
# Required markers are intentionally project-owned and stable.
find_project_root() {
  local d="$SCRIPT_DIR"
  while [[ "$d" != "/" && -n "$d" ]]; do
    if [[ -d "$d/shared/include" && -d "$d/drivers/monado_driver" ]]; then
      printf '%s\n' "$d"
      return 0
    fi
    d="$(dirname "$d")"
  done

  # Runtime package fallback: <package>/bin/drivers/monado_driver/start.sh
  d="$SCRIPT_DIR"
  while [[ "$d" != "/" && -n "$d" ]]; do
    if [[ -n "$XR_MONADO_DEVICE" \
          && -f "$d/devices/$XR_MONADO_DEVICE/$XR_MONADO_DEVICE.env" \
          && -d "$d/bin/drivers/monado_driver" ]]; then
      printf '%s\n' "$d"
      return 0
    fi

    # Generic fallback for future single-device packages. This keeps the runtime
    # driver start script from hard-coding xreal_ultra as the only package shape.
    if [[ -d "$d/bin/drivers/monado_driver" && -d "$d/devices" ]]; then
      local env_files=()
      local candidate=""
      while IFS= read -r candidate; do
        env_files+=("$candidate")
      done < <(find "$d/devices" -mindepth 2 -maxdepth 2 -type f -name '*.env' 2>/dev/null | sort)
      if [[ "${#env_files[@]}" -eq 1 ]]; then
        printf '%s\n' "$d"
        return 0
      fi
    fi
    d="$(dirname "$d")"
  done

  # Local installed source-tree fallback: <root>/bin/drivers/monado_driver/start.sh
  d="$SCRIPT_DIR"
  while [[ "$d" != "/" && -n "$d" ]]; do
    if [[ -d "$d/third_party/monado_driver" && -d "$d/bin/drivers/monado_driver" ]]; then
      printf '%s\n' "$d"
      return 0
    fi
    d="$(dirname "$d")"
  done

  return 1
}

ROOT_PROJECT="$(expand_path "${ROOT_PROJECT:-$(find_project_root || true)}")"
if [[ -z "$ROOT_PROJECT" || ! -d "$ROOT_PROJECT" ]]; then
  fail "cannot determine ROOT_PROJECT. Set ROOT_PROJECT=/home/vlados/src/xr_tracking"
fi

THIRD_PARTY_DIR="$(expand_path "${THIRD_PARTY_DIR:-$ROOT_PROJECT/third_party}")"
MONADO_DIR="$(expand_path "${MONADO_DIR:-$THIRD_PARTY_DIR/monado_driver}")"
BUILD_DIR="$(expand_path "${BUILD_DIR:-$MONADO_DIR/build/xr_tracking_relwithdebinfo}")"
BIN_DIR="$(expand_path "${BIN_DIR:-${XR_BIN_ROOT:-$ROOT_PROJECT/bin}/drivers/monado_driver}")"

# Prefer explicit binary override, then installed binary, then build tree binary.
CANDIDATES=()
if [[ -n "${XR_MONADO_SERVICE_BIN:-}" ]]; then
  CANDIDATES+=("$(expand_path "$XR_MONADO_SERVICE_BIN")")
fi
CANDIDATES+=(
  "$BIN_DIR/monado-service"
  "$BUILD_DIR/src/xrt/targets/service/monado-service"
)

SERVICE_BIN=""
for candidate in "${CANDIDATES[@]}"; do
  if [[ -x "$candidate" ]]; then
    SERVICE_BIN="$candidate"
    break
  fi
done

if [[ -z "$SERVICE_BIN" ]]; then
  echo "[start_monado_driver][ERROR] monado-service not found. Run drivers/monado_driver/scripts/linux/install.sh first." >&2
  for candidate in "${CANDIDATES[@]}"; do
    echo "[start_monado_driver][ERROR] tried: $candidate" >&2
  done
  exit 1
fi

export XR_TRACKING_ROOT="$ROOT_PROJECT"
export XR_TRACKING_RUNTIME_ENABLE="${XR_TRACKING_RUNTIME_ENABLE:-1}"

normalize_compositor_mode() {
  local value="${1:-}"
  value="${value,,}"
  value="${value//-/_}"
  case "$value" in
    "")
      # Backward compatibility: older launchers used XR_MONADO_DIRECT=1 to let
      # Monado try direct mode, otherwise this script forced the XCB fullscreen
      # fallback. Preserve that default while allowing an explicit mode variable.
      if is_truthy "${XR_MONADO_DIRECT:-0}"; then
        printf 'direct\n'
      else
        printf 'xcb_fullscreen\n'
      fi
      ;;
    auto) printf 'auto\n' ;;
    direct|direct_mode|drm|drm_lease) printf 'direct\n' ;;
    xcb|window|windowed) printf 'xcb\n' ;;
    xcb_fullscreen|fullscreen|windowed_fullscreen|extended_sbs|sbs) printf 'xcb_fullscreen\n' ;;
    *)
      echo "[start_monado_driver][ERROR] unknown XR_TRACKING_MONADO_COMPOSITOR_MODE=$1" >&2
      echo "[start_monado_driver][ERROR] expected: auto, direct, xcb, xcb_fullscreen" >&2
      exit 2
      ;;
  esac
}

XR_TRACKING_MONADO_COMPOSITOR_MODE="$(normalize_compositor_mode "${XR_TRACKING_MONADO_COMPOSITOR_MODE:-}")"
case "$XR_TRACKING_MONADO_COMPOSITOR_MODE" in
  auto)
    unset XRT_COMPOSITOR_FORCE_XCB
    unset XRT_COMPOSITOR_XCB_FULLSCREEN
    ;;
  direct)
    unset XRT_COMPOSITOR_FORCE_XCB
    unset XRT_COMPOSITOR_XCB_FULLSCREEN
    ;;
  xcb)
    export XRT_COMPOSITOR_FORCE_XCB=1
    unset XRT_COMPOSITOR_XCB_FULLSCREEN
    ;;
  xcb_fullscreen)
    export XRT_COMPOSITOR_FORCE_XCB=1
    export XRT_COMPOSITOR_XCB_FULLSCREEN=1
    ;;
  *)
    fail "internal compositor mode error: $XR_TRACKING_MONADO_COMPOSITOR_MODE"
    ;;
esac

log "ROOT_PROJECT=$ROOT_PROJECT"
log "MONADO_DIR=$MONADO_DIR"
log "SERVICE_BIN=$SERVICE_BIN"
log "XR_MONADO_DEVICE=$XR_MONADO_DEVICE"
log "XR_TRACKING_RUNTIME_ENABLE=$XR_TRACKING_RUNTIME_ENABLE"
log "XR_TRACKING_MONADO_COMPOSITOR_MODE=$XR_TRACKING_MONADO_COMPOSITOR_MODE"
if [[ -n "${XRT_COMPOSITOR_FORCE_XCB:-}" ]]; then
  log "XRT_COMPOSITOR_FORCE_XCB=$XRT_COMPOSITOR_FORCE_XCB"
fi
if [[ -n "${XRT_COMPOSITOR_XCB_FULLSCREEN:-}" ]]; then
  log "XRT_COMPOSITOR_XCB_FULLSCREEN=$XRT_COMPOSITOR_XCB_FULLSCREEN"
fi
if [[ -n "${XR_TRACKING_MONADO_XCB_OUTPUT:-}" ]]; then
  log "XR_TRACKING_MONADO_XCB_OUTPUT=$XR_TRACKING_MONADO_XCB_OUTPUT"
fi

if [[ -n "${XR_TRACKING_MONADO_XCB_OUTPUT:-}" ]]; then
  if [[ "$XR_TRACKING_MONADO_COMPOSITOR_MODE" != "xcb" ]]; then
    fail "XR_TRACKING_MONADO_XCB_OUTPUT requires XR_TRACKING_MONADO_COMPOSITOR_MODE=xcb, not $XR_TRACKING_MONADO_COMPOSITOR_MODE"
  fi
  run_service_with_xcb_output_placement "$XR_TRACKING_MONADO_XCB_OUTPUT" "$@"
  exit $?
fi

exec "$SERVICE_BIN" "$@"
