#!/usr/bin/env bash
set -euo pipefail

log() { echo "[start_override_controller] $*" >&2; }
fatal() { echo "[start_override_controller][ERROR] $*" >&2; exit 1; }

expand_tilde() {
  local value="${1:-}"
  case "$value" in
    "~") printf '%s\n' "$HOME" ;;
    "~/"*) printf '%s\n' "$HOME/${value#"~/"}" ;;
    *) printf '%s\n' "$value" ;;
  esac
}

ROOT_PROJECT="${ROOT_PROJECT:-$HOME/src/xr_tracking}"                                       # project root; can be overridden when running from another checkout
ROOT_PROJECT="$(expand_tilde "$ROOT_PROJECT")"                                              # normalize ~/ in project root path

OVERRIDE_CONTROLLER_DIR="${OVERRIDE_CONTROLLER_DIR:-$ROOT_PROJECT/override_controller}"     # override_controller source directory
OVERRIDE_CONTROLLER_DIR="$(expand_tilde "$OVERRIDE_CONTROLLER_DIR")"                        # normalize ~/ in override_controller source path

INSTALL_BIN_DIR="${INSTALL_BIN_DIR:-$ROOT_PROJECT/bin/override_controller}"                 # installed override_controller binary directory
INSTALL_BIN_DIR="$(expand_tilde "$INSTALL_BIN_DIR")"                                        # normalize ~/ in install binary directory

OVERRIDE_CONTROLLER_BIN="${OVERRIDE_CONTROLLER_BIN:-$INSTALL_BIN_DIR/override_controller}"  # override_controller executable path
OVERRIDE_CONTROLLER_BIN="$(expand_tilde "$OVERRIDE_CONTROLLER_BIN")"                        # normalize ~/ in executable path

CONFIG_DIR="${CONFIG_DIR:-${XDG_CONFIG_HOME:-$HOME/.config}/xr_tracking/override_controller}" # directory with trained controller bindings/configs
CONFIG_DIR="$(expand_tilde "$CONFIG_DIR")"                                                  # normalize ~/ in config directory

CONFIG_PATH="${CONFIG_PATH:-}"                                                              # optional explicit config file path; empty means use CONFIG_DIR/CONFIG_NAME
if [[ -n "$CONFIG_PATH" ]]; then
  CONFIG_PATH="$(expand_tilde "$CONFIG_PATH")"                                              # normalize ~/ in explicit config path
fi

CONFIG_NAME="${CONFIG_NAME:-default}"                                                       # trained config profile name used with --train
AUTO_BUILD="${AUTO_BUILD:-1}"                                                               # build override_controller automatically when binary is missing or stale
TRAIN="${TRAIN:-0}"                                                                         # run interactive training mode when set to 1
LIST_DEVICES="${LIST_DEVICES:-0}"                                                           # list detected input devices and exit when set to 1
NON_INTERACTIVE="${NON_INTERACTIVE:-0}"                                                     # disable interactive prompts when set to 1
VERBOSE="${VERBOSE:-0}"                                                                     # enable verbose override_controller logging when set to 1
USE_SUDO="${USE_SUDO:-0}"                                                                   # run binary through sudo for evdev access when set to 1
GRAB_DEVICES="${GRAB_DEVICES:-${OVERRIDE_CONTROLLER_GRAB_DEVICES:-1}}"                      # grab evdev devices exclusively to prevent duplicate OS/game input
REATTACH_DEVICES="${REATTACH_DEVICES:-${OVERRIDE_CONTROLLER_REATTACH_DEVICES:-0}}"          # periodically rescan/reopen input devices after disconnect/reconnect
REATTACH_INTERVAL_MS="${REATTACH_INTERVAL_MS:-${OVERRIDE_CONTROLLER_REATTACH_INTERVAL_MS:-3000}}" # device reattach/rescan interval in milliseconds

EVENT_WAIT_MAX_MS="${EVENT_WAIT_MAX_MS:-${OVERRIDE_CONTROLLER_EVENT_WAIT_MAX_MS:-20}}"      # maximum evdev poll wait before publishing current state
REL_AXIS_HOLD_MS="${REL_AXIS_HOLD_MS:-${OVERRIDE_CONTROLLER_REL_AXIS_HOLD_MS:-0}}"          # legacy hold time for relative-axis events; 0 disables it
REL_BUTTON_HOLD_MS="${REL_BUTTON_HOLD_MS:-${OVERRIDE_CONTROLLER_REL_BUTTON_HOLD_MS:-0}}"    # legacy hold time for relative-button/D-pad pulses; 0 disables it
BUTTON_HOLD_MS="${BUTTON_HOLD_MS:-${OVERRIDE_CONTROLLER_BUTTON_HOLD_MS:-0}}"                # legacy minimum hold time for button presses; 0 disables it
BUTTON_RELEASE_GRACE_MS="${BUTTON_RELEASE_GRACE_MS:-${OVERRIDE_CONTROLLER_BUTTON_RELEASE_GRACE_MS:-0}}" # legacy release grace bridge after button release; 0 disables it

PULSE_MODE="${PULSE_MODE:-${OVERRIDE_CONTROLLER_PULSE_MODE:-1}}"                            # enable pulse-source filtering for controllers that emit repeated short pulses

DPAD_PULSE_GAP_MS="${DPAD_PULSE_GAP_MS:-${OVERRIDE_CONTROLLER_DPAD_PULSE_GAP_MS:-120}}"     # expected maximum gap between D-pad pulse events
DPAD_RELEASE_MS="${DPAD_RELEASE_MS:-${OVERRIDE_CONTROLLER_DPAD_RELEASE_MS:-130}}"           # virtual D-pad release timeout after the last pulse

BUTTON_PULSE_GAP_MS="${BUTTON_PULSE_GAP_MS:-${OVERRIDE_CONTROLLER_BUTTON_PULSE_GAP_MS:-150}}" # expected maximum gap between button pulse events
BUTTON_RELEASE_MS="${BUTTON_RELEASE_MS:-${OVERRIDE_CONTROLLER_BUTTON_RELEASE_MS:-160}}"     # virtual button release timeout after the last pulse

BUTTON_PULSE_STARTUP_MS="${BUTTON_PULSE_STARTUP_MS:-${OVERRIDE_CONTROLLER_BUTTON_PULSE_STARTUP_MS:-0}}" # optional early startup window for longer pulse bridging; 0 disables it
BUTTON_PULSE_STARTUP_RELEASE_MS="${BUTTON_PULSE_STARTUP_RELEASE_MS:-${OVERRIDE_CONTROLLER_BUTTON_PULSE_STARTUP_RELEASE_MS:-0}}" # release timeout used only during the startup window
BUTTON_PULSE_STARTUP_TYPES="${BUTTON_PULSE_STARTUP_TYPES:-${OVERRIDE_CONTROLLER_BUTTON_PULSE_STARTUP_TYPES:-trigger}}" # comma-separated action types that may use startup pulse bridging

HOLD_TOGGLE_DEBOUNCE_MS="${HOLD_TOGGLE_DEBOUNCE_MS:-${OVERRIDE_CONTROLLER_HOLD_TOGGLE_DEBOUNCE_MS:-100}}" # debounce window for click-to-toggle virtual long-press mode


# ControllerInput publisher settings. These must match xr_runtime_adapter
# --controller-input-* settings. Keep /tmp/tracking_streams.json as the default
# because xr_runtime_adapter uses the same default for external input streams.
# Set CONTROLLER_INPUT_REGISTRY=/tmp/runtime_tracking_streams.json only if the
# adapter is configured with the same --controller-input-registry.
CONTROLLER_INPUT_REGISTRY="${CONTROLLER_INPUT_REGISTRY:-${XR_CONTROLLER_INPUT_REGISTRY:-/tmp/tracking_streams.json}}" # registry JSON used to publish controller_input stream metadata
CONTROLLER_INPUT_REGISTRY="$(expand_tilde "$CONTROLLER_INPUT_REGISTRY")"                    # normalize ~/ in controller_input registry path
CONTROLLER_INPUT_STREAM="${CONTROLLER_INPUT_STREAM:-controller_input}"                      # published ControllerInputV2 stream name
CONTROLLER_INPUT_SHM_NAME="${CONTROLLER_INPUT_SHM_NAME:-controller_input}"                  # shared-memory object name for controller_input payloads
CONTROLLER_INPUT_RATE_HZ="${CONTROLLER_INPUT_RATE_HZ:-90}"                                  # controller_input publish rate expected by runtime adapter/SteamVR path
CONTROLLER_INPUT_SLOTS="${CONTROLLER_INPUT_SLOTS:-32}"                                      # ring-buffer slot count for controller_input SHM
FIX_REGISTRY_PERMISSIONS="${FIX_REGISTRY_PERMISSIONS:-1}"                                   # fix stale root-owned registry/SHM files before publishing when possible

[[ -d "$ROOT_PROJECT" ]] || fatal "ROOT_PROJECT not found: $ROOT_PROJECT"
[[ -d "$OVERRIDE_CONTROLLER_DIR" ]] || fatal "override_controller source dir not found: $OVERRIDE_CONTROLLER_DIR"

print_input_permission_hint_if_needed() {
  [[ "$(uname -s)" == "Linux" ]] || return 0
  [[ "${EUID:-$(id -u)}" -eq 0 ]] && return 0
  [[ -d /dev/input ]] || return 0

  local readable_count
  readable_count=$(find /dev/input -maxdepth 1 -type c -name 'event*' -readable 2>/dev/null | wc -l | tr -d ' ')
  if [[ "${readable_count:-0}" == "0" ]]; then
    if [[ "$USE_SUDO" == "1" ]]; then
      echo "[start_override_controller] Current user has no evdev access, but USE_SUDO=1 is enabled; continuing via sudo."
    else
      echo "[start_override_controller] No readable /dev/input/event* devices..."
    fi
    log "Permanent fix: sudo usermod -aG input $USER   then log out/in, or run: newgrp input"
    log "Temporary test: USE_SUDO=1 $0   or: sudo setfacl -m u:$USER:rw /dev/input/event*"
  fi
}

fix_runtime_file_permissions_if_needed() {
  [[ "$FIX_REGISTRY_PERMISSIONS" == "1" ]] || return 0
  [[ "${EUID:-$(id -u)}" -eq 0 ]] && return 0

  local uid gid
  uid="$(id -u)"
  gid="$(id -g)"

  local registry_lock="${CONTROLLER_INPUT_REGISTRY}.lock"
  local shm_leaf="${CONTROLLER_INPUT_SHM_NAME#/}"
  local shm_path="/dev/shm/${shm_leaf}"

  local need_sudo=0
  if [[ -e "$CONTROLLER_INPUT_REGISTRY" && ! -w "$CONTROLLER_INPUT_REGISTRY" ]]; then
    need_sudo=1
  fi
  if [[ -e "$registry_lock" && ! -w "$registry_lock" ]]; then
    need_sudo=1
  fi
  if [[ -e "$shm_path" && ! -w "$shm_path" ]]; then
    need_sudo=1
  fi

  [[ "$need_sudo" == "1" ]] || return 0
  command -v sudo >/dev/null 2>&1 || {
    log "Registry/SHM files are not writable and sudo is not available."
    log "Manual cleanup: sudo rm -f '$registry_lock' && sudo chown $USER:$USER '$CONTROLLER_INPUT_REGISTRY' '$shm_path' 2>/dev/null || true"
    return 0
  }

  log "Fixing stale root-owned registry/SHM files if present"
  sudo rm -f "$registry_lock" 2>/dev/null || true
  if [[ -e "$CONTROLLER_INPUT_REGISTRY" ]]; then
    sudo chown "$uid:$gid" "$CONTROLLER_INPUT_REGISTRY" 2>/dev/null || true
  fi
  if [[ -e "$shm_path" ]]; then
    sudo chown "$uid:$gid" "$shm_path" 2>/dev/null || true
  fi
}

if [[ ! -x "$OVERRIDE_CONTROLLER_BIN" ]]; then
  if [[ "$AUTO_BUILD" == "1" ]]; then
    log "Binary not found, building: $OVERRIDE_CONTROLLER_BIN"
    "$OVERRIDE_CONTROLLER_DIR/scripts/linux/install_override_controller.sh"
  else
    fatal "Binary not found: $OVERRIDE_CONTROLLER_BIN (set AUTO_BUILD=1 or run install script)"
  fi
elif [[ "$AUTO_BUILD" == "1" ]]; then
  newer_source="$(find "$OVERRIDE_CONTROLLER_DIR" \
    -type f \
    \( -name '*.cpp' -o -name '*.hpp' -o -name 'CMakeLists.txt' -o -name '*.cmake' \) \
    -newer "$OVERRIDE_CONTROLLER_BIN" -print -quit 2>/dev/null || true)"
  if [[ -n "$newer_source" ]]; then
    log "Source is newer than binary, rebuilding override_controller"
    "$OVERRIDE_CONTROLLER_DIR/scripts/linux/install_override_controller.sh"
  fi
fi

args=("--config-dir" "$CONFIG_DIR")
args+=("--publish-registry" "$CONTROLLER_INPUT_REGISTRY")
args+=("--publish-stream" "$CONTROLLER_INPUT_STREAM")
args+=("--publish-shm-name" "$CONTROLLER_INPUT_SHM_NAME")
args+=("--publish-rate-hz" "$CONTROLLER_INPUT_RATE_HZ")
args+=("--publish-slots" "$CONTROLLER_INPUT_SLOTS")
args+=("--grab-devices" "$GRAB_DEVICES")
args+=("--reattach-devices" "$REATTACH_DEVICES")
args+=("--reattach-interval-ms" "$REATTACH_INTERVAL_MS")
args+=("--event-wait-max-ms" "$EVENT_WAIT_MAX_MS")
args+=("--rel-axis-hold-ms" "$REL_AXIS_HOLD_MS")
args+=("--rel-button-hold-ms" "$REL_BUTTON_HOLD_MS")
args+=("--button-hold-ms" "$BUTTON_HOLD_MS")
args+=("--button-release-grace-ms" "$BUTTON_RELEASE_GRACE_MS")
args+=("--pulse-mode" "$PULSE_MODE")
args+=("--dpad-pulse-gap-ms" "$DPAD_PULSE_GAP_MS")
args+=("--dpad-release-ms" "$DPAD_RELEASE_MS")
args+=("--button-pulse-gap-ms" "$BUTTON_PULSE_GAP_MS")
args+=("--button-release-ms" "$BUTTON_RELEASE_MS")
args+=("--button-pulse-startup-ms" "$BUTTON_PULSE_STARTUP_MS")
args+=("--button-pulse-startup-release-ms" "$BUTTON_PULSE_STARTUP_RELEASE_MS")
args+=("--button-pulse-startup-types" "$BUTTON_PULSE_STARTUP_TYPES")
args+=("--hold-toggle-debounce-ms" "$HOLD_TOGGLE_DEBOUNCE_MS")

if [[ -n "$CONFIG_PATH" ]]; then
  args+=("--config" "$CONFIG_PATH")
fi

if [[ "$TRAIN" == "1" ]]; then
  args+=("--train" "--name" "$CONFIG_NAME")
fi

if [[ "$LIST_DEVICES" == "1" ]]; then
  args+=("--list-devices")
fi

if [[ "$NON_INTERACTIVE" == "1" ]]; then
  args+=("--non-interactive")
fi

if [[ "$VERBOSE" == "1" ]]; then
  args+=("--verbose")
fi

log "ROOT_PROJECT=$ROOT_PROJECT"
log "OVERRIDE_CONTROLLER_DIR=$OVERRIDE_CONTROLLER_DIR"
log "BIN=$OVERRIDE_CONTROLLER_BIN"
log "CONFIG_DIR=$CONFIG_DIR"
if [[ -n "$CONFIG_PATH" ]]; then
  log "CONFIG_PATH=$CONFIG_PATH"
fi
log "CONTROLLER_INPUT_REGISTRY=$CONTROLLER_INPUT_REGISTRY"
log "CONTROLLER_INPUT_STREAM=$CONTROLLER_INPUT_STREAM"
log "CONTROLLER_INPUT_SHM_NAME=$CONTROLLER_INPUT_SHM_NAME"
log "CONTROLLER_INPUT_RATE_HZ=$CONTROLLER_INPUT_RATE_HZ CONTROLLER_INPUT_SLOTS=$CONTROLLER_INPUT_SLOTS"
log "GRAB_DEVICES=$GRAB_DEVICES REATTACH_DEVICES=$REATTACH_DEVICES REATTACH_INTERVAL_MS=$REATTACH_INTERVAL_MS"
log "HOLD_MS rel_axis=$REL_AXIS_HOLD_MS rel_button=$REL_BUTTON_HOLD_MS button=$BUTTON_HOLD_MS release_grace=$BUTTON_RELEASE_GRACE_MS"
log "PULSE_MODE=$PULSE_MODE dpad_gap=$DPAD_PULSE_GAP_MS dpad_release=$DPAD_RELEASE_MS button_gap=$BUTTON_PULSE_GAP_MS button_release=$BUTTON_RELEASE_MS button_startup_ms=$BUTTON_PULSE_STARTUP_MS button_startup_release_ms=$BUTTON_PULSE_STARTUP_RELEASE_MS button_startup_types=$BUTTON_PULSE_STARTUP_TYPES"
log "HOLD_TOGGLE_DEBOUNCE_MS=$HOLD_TOGGLE_DEBOUNCE_MS"
log "EVENT_WAIT_MAX_MS=$EVENT_WAIT_MAX_MS REL_AXIS_HOLD_MS=$REL_AXIS_HOLD_MS REL_BUTTON_HOLD_MS=$REL_BUTTON_HOLD_MS BUTTON_HOLD_MS=$BUTTON_HOLD_MS BUTTON_RELEASE_GRACE_MS=$BUTTON_RELEASE_GRACE_MS"
log "TRAIN=$TRAIN LIST_DEVICES=$LIST_DEVICES NON_INTERACTIVE=$NON_INTERACTIVE VERBOSE=$VERBOSE USE_SUDO=$USE_SUDO"

print_input_permission_hint_if_needed
fix_runtime_file_permissions_if_needed

if [[ "$USE_SUDO" == "1" && "${EUID:-$(id -u)}" -ne 0 ]]; then
  command -v sudo >/dev/null 2>&1 || fatal "sudo not found; cannot run with USE_SUDO=1"
  log "Running override_controller via sudo. Config path remains: $CONFIG_DIR"
  set +e
  sudo "$OVERRIDE_CONTROLLER_BIN" "${args[@]}" "$@"
  status=$?
  set -e
  if [[ -d "$CONFIG_DIR" ]]; then
    sudo chown -R "$(id -u):$(id -g)" "$CONFIG_DIR" || true
  fi
  if [[ "$FIX_REGISTRY_PERMISSIONS" == "1" ]]; then
    registry_lock="${CONTROLLER_INPUT_REGISTRY}.lock"
    shm_leaf="${CONTROLLER_INPUT_SHM_NAME#/}"
    sudo chown "$(id -u):$(id -g)" "$CONTROLLER_INPUT_REGISTRY" "$registry_lock" "/dev/shm/${shm_leaf}" 2>/dev/null || true
  fi
  exit "$status"
fi

exec "$OVERRIDE_CONTROLLER_BIN" "${args[@]}" "$@"
