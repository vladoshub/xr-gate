#!/usr/bin/env bash
# Keep the SteamVR SBS window on the selected X11/XWayland monitor.
set -u

log() { printf '[move_openvr_window] %s\n' "$*" >&2; }
fail() { log "ERROR: $*"; exit 1; }

case "${1:-}" in
  ""|--choose) ;;
  --help|-h)
    echo "Usage: move_openvr_window.sh [--choose]"
    echo "Select a headset display and move the SteamVR window to it. Ctrl+C stops the watcher."
    echo "Use --choose to choose a different display or set XR_OPENVR_WINDOW_OUTPUT."
    exit 0
    ;;
  *) fail "Unknown argument: $1. Use --help for usage." ;;
esac

for tool in xrandr xdotool pgrep python3 awk; do
  command -v "$tool" >/dev/null 2>&1 || \
    fail "Missing $tool. On Ubuntu install: sudo apt install xdotool x11-xserver-utils procps python3"
done
[[ -n "${DISPLAY:-}" ]] || fail "DISPLAY is unset. Run this from the graphical session."

# Prefer the configuration directory registered with OpenVR; the explicit
# override is useful when Steam is installed in a nonstandard location.
if ! size="$(python3 - "${XR_OPENVR_STEAMVR_SETTINGS:-}" "${XR_OPENVRPATHS_FILE:-}" <<'PY'
import json
import sys
from pathlib import Path

home = Path.home()
explicit, registry_override = sys.argv[1:]
paths = []
if explicit:
    paths.append(Path(explicit).expanduser())
else:
    registry = Path(registry_override).expanduser() if registry_override else home / ".config/openvr/openvrpaths.vrpath"
    try:
        paths.extend(Path(folder).expanduser() / "steamvr.vrsettings"
                     for folder in json.loads(registry.read_text()).get("config", []))
    except (OSError, ValueError, TypeError):
        pass
    paths.extend([home / ".local/share/Steam/config/steamvr.vrsettings",
                  home / ".steam/steam/config/steamvr.vrsettings"])
for path in paths:
    try:
        xr = json.loads(path.read_text())["xr_tracking"]
        width, height = int(xr["windowWidth"]), int(xr["windowHeight"])
        if width > 0 and height > 0:
            print(width, height)
            break
    except (OSError, ValueError, KeyError, TypeError):
        continue
else:
    print("Could not read the XR Gate window size from SteamVR settings. Register the driver first.", file=sys.stderr)
    sys.exit(1)
PY
)"; then
  exit 1
fi
read -r expected_width expected_height <<< "$size"

if ! monitor_list="$(xrandr --listmonitors)"; then
  fail "Cannot list X11/XWayland monitors. Check DISPLAY and the graphical session."
fi
mapfile -t monitors < <(awk 'NR > 1 {print $NF}' <<< "$monitor_list")
((${#monitors[@]})) || fail "No active X11/XWayland monitors found."

config_dir="${XDG_CONFIG_HOME:-$HOME/.config}/xr-gate"
output_file="$config_dir/steamvr-window-output"
output="${XR_OPENVR_WINDOW_OUTPUT:-}"
if [[ "${1:-}" != --choose && -z "$output" && -f "$output_file" ]]; then
  IFS= read -r output < "$output_file" || true
fi

if [[ -n "$output" ]]; then
  active=0
  for monitor in "${monitors[@]}"; do
    [[ "$monitor" == "$output" ]] && active=1
  done
  if (( ! active )); then
    if [[ -n "${XR_OPENVR_WINDOW_OUTPUT:-}" ]]; then
      fail "Monitor $output is not active. Check xrandr --listmonitors."
    fi
    log "Saved display $output is not active; choose another display."
    output=""
  fi
fi

if [[ -z "$output" ]]; then
  if ((${#monitors[@]} == 1)); then
    output="${monitors[0]}"
  else
    [[ -t 0 ]] || fail "Multiple displays are available. Run interactively or set XR_OPENVR_WINDOW_OUTPUT."
    log "Select the headset display:"
    printf '%s\n' "$monitor_list"
    select selected in "${monitors[@]}"; do
      if [[ -n "$selected" ]]; then
        output="$selected"
        break
      fi
      log "Enter a display number."
    done
    [[ -n "$output" ]] || fail "No display selected."
  fi
  mkdir -p "$config_dir" || fail "Cannot create $config_dir"
  printf '%s\n' "$output" > "$output_file" || fail "Cannot save the selected display"
fi

log "Headset display: $output; SteamVR window: ${expected_width}x${expected_height}."
log "Waiting for SteamVR. Press Ctrl+C to stop."
last_move=""
last_problem=""

while true; do
  # Re-read XWayland coordinates after display rearrangement, scale or mode changes.
  geometry="$(xrandr --listmonitors 2>/dev/null | awk -v name="$output" '$NF == name {print $3; exit}')"
  if [[ ! "$geometry" =~ ^([0-9]+)(/[0-9]+)?x([0-9]+)(/[0-9]+)?([+-][0-9]+)([+-][0-9]+)$ ]]; then
    if [[ "$last_problem" != monitor ]]; then
      log "Display $output is not active; waiting for it to reconnect."
      last_problem=monitor
    fi
    last_move=""
    sleep 1
    continue
  fi
  # xdotool expects signed pixel coordinates, including negative monitor offsets.
  target_x="${BASH_REMATCH[5]}"
  target_y="${BASH_REMATCH[6]}"
  matches=()
  while read -r pid; do
    [[ -n "$pid" ]] || continue
    while read -r window; do
      [[ "$window" =~ ^[0-9]+$ ]] || continue
      window_geometry="$(xdotool getwindowgeometry --shell "$window" 2>/dev/null)" || continue
      width="$(awk -F= '$1 == "WIDTH" {print $2}' <<< "$window_geometry")"
      height="$(awk -F= '$1 == "HEIGHT" {print $2}' <<< "$window_geometry")"
      if [[ "$width" == "$expected_width" && "$height" == "$expected_height" ]]; then
        matches+=("$pid:$window")
      fi
    done < <(xdotool search --onlyvisible --pid "$pid" 2>/dev/null || true)
  done < <(pgrep -u "$(id -u)" -x vrcompositor 2>/dev/null || true)

  if ((${#matches[@]} == 1)); then
    window="${matches[0]#*:}"
    move_key="${matches[0]}:$target_x:$target_y"
    if [[ "$move_key" != "$last_move" ]]; then
      if xdotool windowmove "$window" "$target_x" "$target_y"; then
        log "Moved SteamVR window $window to $output ($target_x, $target_y)."
        last_move="$move_key"
        last_problem=""
      elif [[ "$last_problem" != move ]]; then
        log "Could not move window $window. Your compositor may block XWayland positioning."
        last_problem=move
      fi
    fi
  else
    last_move=""
    if ((${#matches[@]} > 1)) && [[ "$last_problem" != ambiguous ]]; then
      log "Multiple matching SteamVR windows found; not moving an ambiguous window."
      last_problem=ambiguous
    elif ((${#matches[@]} == 0)) && [[ "$last_problem" == ambiguous ]]; then
      last_problem=""
    fi
  fi
  sleep 1
done
