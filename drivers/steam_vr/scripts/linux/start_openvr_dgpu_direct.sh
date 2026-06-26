#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

log() { echo "[start_openvr_dgpu_direct] $*" >&2; }
fatal() { echo "[start_openvr_dgpu_direct][ERROR] $*" >&2; exit 1; }

expand_tilde() {
  local value="$1"
  case "$value" in
    "~") printf '%s\n' "$HOME" ;;
    "~/"*) printf '%s\n' "$HOME/${value#"~/"}" ;;
    *) printf '%s\n' "$value" ;;
  esac
}

resolve_runtime_root() {
  local explicit="${XR_PACKAGE_ROOT:-${XR_ROOT_PROJECT:-${ROOT_PROJECT:-}}}"
  if [[ -n "$explicit" ]]; then
    explicit="$(expand_tilde "$explicit")"
    if [[ -d "$explicit" ]]; then
      printf '%s\n' "$explicit"
      return 0
    fi
  fi

  # Source-tree layout:
  #   <repo>/drivers/steam_vr/scripts/linux/start_openvr_dgpu_direct.sh
  local source_candidate
  source_candidate="$(cd "$SCRIPT_DIR/../../../.." >/dev/null 2>&1 && pwd || true)"
  if [[ -n "$source_candidate" && -d "$source_candidate/drivers/openvr_driver" ]]; then
    printf '%s\n' "$source_candidate"
    return 0
  fi

  # Package layout:
  #   <package>/bin/scripts/drivers/steam_vr/start_openvr_dgpu_direct.sh
  local package_candidate
  package_candidate="$(cd "$SCRIPT_DIR/../../../.." >/dev/null 2>&1 && pwd || true)"
  if [[ -n "$package_candidate" && -d "$package_candidate/bin/drivers" ]]; then
    printf '%s\n' "$package_candidate"
    return 0
  fi

  fatal "cannot resolve runtime root from SCRIPT_DIR=$SCRIPT_DIR"
}

resolve_package_root() {
  local root="$1"
  if [[ -d "$root/bin/drivers" ]]; then
    printf '%s\n' "$root"
    return 0
  fi
  if [[ -d "$root/out/xreal_ultra/bin/drivers" ]]; then
    printf '%s\n' "$root/out/xreal_ultra"
    return 0
  fi
  printf '%s\n' "$root"
}

resolve_register_script() {
  local project_root="$1"
  local package_root="$2"
  local explicit="${XR_OPENVR_REGISTER_SCRIPT:-${OPENVR_REGISTER_DRIVER_SCRIPT:-}}"
  if [[ -n "$explicit" ]]; then
    explicit="$(expand_tilde "$explicit")"
    if [[ -x "$explicit" ]]; then
      printf '%s\n' "$explicit"
      return 0
    fi
    fatal "XR_OPENVR_REGISTER_SCRIPT is set but not executable: $explicit"
  fi

  local candidate
  for candidate in \
    "$package_root/bin/scripts/drivers/openvr_driver/register_driver.sh" \
    "$project_root/drivers/openvr_driver/scripts/register_driver.sh" \
    "$project_root/out/xreal_ultra/bin/scripts/drivers/openvr_driver/register_driver.sh"; do
    if [[ -x "$candidate" ]]; then
      printf '%s\n' "$candidate"
      return 0
    fi
  done
  fatal "OpenVR register_driver.sh not found"
}

FREQ_ARG="${1:-}"
if [[ "$FREQ_ARG" =~ ^[0-9]+([.][0-9]+)?$ ]]; then
  shift
else
  FREQ_ARG=""
fi

normalize_hz() {
  local value="${1:-60}"
  python3 - "$value" <<'PY'
import math
import sys
text = sys.argv[1].strip()
try:
    value = float(text)
except ValueError:
    print(f"[ERROR] Unsupported frequency: {text}", file=sys.stderr)
    sys.exit(2)
if not math.isfinite(value) or abs(value - round(value)) > 1e-6:
    print(f"[ERROR] Unsupported frequency: {text}; expected integer Hz", file=sys.stderr)
    sys.exit(2)
hz = int(round(value))
if hz < 30 or hz > 240:
    print(f"[ERROR] Unsupported frequency: {hz}; expected 30..240 Hz", file=sys.stderr)
    sys.exit(2)
print(hz)
PY
}

RUNTIME_ROOT="$(resolve_runtime_root)"
PACKAGE_ROOT="$(resolve_package_root "$RUNTIME_ROOT")"
if [[ -d "$PACKAGE_ROOT/bin/drivers" ]]; then
  DRIVERS_ROOT_DEFAULT="$PACKAGE_ROOT/bin/drivers"
else
  DRIVERS_ROOT_DEFAULT="$RUNTIME_ROOT/bin/drivers"
fi
DRIVERS_ROOT="$(expand_tilde "${XR_OPENVR_DRIVERS_ROOT:-${DRIVERS_ROOT:-$DRIVERS_ROOT_DEFAULT}}")"
XR_OUT="${XR_OPENVR_DGPU_OUTPUT:-${XR_OUT:-DP-4}}"
XR_MODE="${XR_OPENVR_DGPU_MODE:-3840x1080}"
OPENVR_FREQ_RAW="${XR_OPENVR_DISPLAY_FREQUENCY_HZ:-${XR_OPENVR_DGPU_FREQ_HZ:-${FREQ_ARG:-60}}}"
OPENVR_FREQ="$(normalize_hz "$OPENVR_FREQ_RAW")"
XR_RATE="${XR_OPENVR_DGPU_RATE_HZ:-$OPENVR_FREQ}"
OPENVR_MODE="${XR_OPENVR_DISPLAY_MODE:-direct}"
OPENVR_DEVICE="${XR_OPENVR_DEVICE:-${XR_DEVICE_TARGET:-${XR_TARGET_DEVICE:-xreal_ultra}}}"
DRIVER_DIR_NAME="${XR_OPENVR_DRIVER_DIR_NAME:-openvr_driver_${OPENVR_FREQ}HZ}"
STOP_STEAM="${XR_OPENVR_STOP_STEAM:-auto}"
STOP_STEAM="${STOP_STEAM,,}"
case "$STOP_STEAM" in
  1|true|yes|on) STOP_STEAM="1" ;;
  0|false|no|off) STOP_STEAM="0" ;;
  auto) ;;
  *) fatal "unsupported XR_OPENVR_STOP_STEAM=$STOP_STEAM; expected auto, 1, or 0" ;;
esac
SET_DISPLAY="${XR_OPENVR_SET_DISPLAY:-1}"
CLEAR_LOGS="${XR_OPENVR_CLEAR_LOGS:-0}"
REGISTER_METHOD="${XR_OPENVR_REGISTER_METHOD:-manual}"
RUNTIME_MODE="${XR_OPENVR_RUNTIME_MODE:-steamvr}"
GPU_VENDOR="${XR_OPENVR_DGPU_VENDOR:-${XR_OPENVR_GPU_VENDOR:-nvidia}}"
GPU_VENDOR="${GPU_VENDOR,,}"
case "$GPU_VENDOR" in
  nvidia|amd|auto) ;;
  *) fatal "unsupported XR_OPENVR_DGPU_VENDOR=$GPU_VENDOR; expected nvidia, amd, or auto" ;;
esac
AMD_DRI_PRIME="${XR_OPENVR_AMD_DRI_PRIME:-${XR_OPENVR_DRI_PRIME:-}}"

# Prefer the register script bundled with the selected driver variant. This avoids
# accidentally using an older generic script from bin/scripts when out/ has
# multiple OpenVR driver packages (60HZ/90HZ, direct/extended_sbs).
SELECTED_REGISTER_SCRIPT="$DRIVERS_ROOT/$DRIVER_DIR_NAME/scripts/register_driver.sh"
if [[ -x "$SELECTED_REGISTER_SCRIPT" ]]; then
  REGISTER_SCRIPT="$SELECTED_REGISTER_SCRIPT"
else
  REGISTER_SCRIPT="$(resolve_register_script "$RUNTIME_ROOT" "$PACKAGE_ROOT")"
fi

# What to launch after display and driver setup:
#   steamvr - start Steam and immediately open SteamVR (default)
#   steam   - start only the Steam client with the same Vulkan/GPU env;
#             launching SteamVR or a VR game later from this Steam process
#             will inherit that env
#   none    - only configure display/register driver and exit
# Backward compatibility: XR_OPENVR_LAUNCH_STEAMVR=0 maps to none unless
# XR_OPENVR_LAUNCH_MODE is explicitly set.
if [[ -n "${XR_OPENVR_LAUNCH_MODE:-}" ]]; then
  LAUNCH_MODE="${XR_OPENVR_LAUNCH_MODE,,}"
elif [[ "${XR_OPENVR_LAUNCH_STEAMVR:-1}" == "0" ]]; then
  LAUNCH_MODE="none"
else
  LAUNCH_MODE="steamvr"
fi
LAUNCH_MODE="${LAUNCH_MODE//-/_}"
case "$LAUNCH_MODE" in
  steamvr|steam|none) ;;
  no_launch|configure_only|setup_only) LAUNCH_MODE="none" ;;
  *) fatal "unsupported XR_OPENVR_LAUNCH_MODE=$LAUNCH_MODE; expected steamvr, steam, or none" ;;
esac

if [[ "$OPENVR_MODE" != "direct" ]]; then
  fatal "this launcher is direct-mode only; got XR_OPENVR_DISPLAY_MODE=$OPENVR_MODE"
fi

find_nvidia_icd() {
  local candidate
  for candidate in \
    /usr/share/vulkan/icd.d/nvidia_icd.json \
    /etc/vulkan/icd.d/nvidia_icd.json \
    /usr/share/vulkan/icd.d/*nvidia*.json \
    /etc/vulkan/icd.d/*nvidia*.json; do
    if [[ -f "$candidate" ]]; then
      printf '%s\n' "$candidate"
      return 0
    fi
  done
  return 1
}

find_amd_icd() {
  local candidate
  # Prefer Mesa RADV on Ubuntu. AMDVLK/AMDGPU-PRO ICDs are accepted when installed.
  for candidate in \
    /usr/share/vulkan/icd.d/radeon_icd.x86_64.json \
    /etc/vulkan/icd.d/radeon_icd.x86_64.json \
    /usr/share/vulkan/icd.d/amd_icd64.json \
    /etc/vulkan/icd.d/amd_icd64.json \
    /usr/share/vulkan/icd.d/amdvlk64.json \
    /etc/vulkan/icd.d/amdvlk64.json \
    /usr/share/vulkan/icd.d/amd_pro_icd64.json \
    /etc/vulkan/icd.d/amd_pro_icd64.json \
    /usr/share/vulkan/icd.d/*radeon*x86_64*.json \
    /etc/vulkan/icd.d/*radeon*x86_64*.json \
    /usr/share/vulkan/icd.d/*amd*64*.json \
    /etc/vulkan/icd.d/*amd*64*.json; do
    if [[ -f "$candidate" ]]; then
      printf '%s\n' "$candidate"
      return 0
    fi
  done
  return 1
}

resolve_gpu_icd() {
  local vendor="$1"
  local explicit="${VK_ICD_FILENAMES:-${XR_OPENVR_VK_ICD_FILENAMES:-}}"

  if [[ -n "$explicit" ]]; then
    printf '%s\n' "$explicit"
    return 0
  fi

  case "$vendor" in
    nvidia)
      find_nvidia_icd
      ;;
    amd)
      find_amd_icd
      ;;
    auto)
      find_nvidia_icd || find_amd_icd
      ;;
    *)
      return 1
      ;;
  esac
}

stop_steamvr_processes() {
  pkill -f vrserver || true
  pkill -f vrcompositor || true
  pkill -f vrmonitor || true
  pkill -f vrdashboard || true
  pkill -f vrwebhelper || true
}

wait_for_steam_shutdown() {
  local deadline=$((SECONDS + 12))
  while (( SECONDS < deadline )); do
    if ! pgrep -u "$USER" -f 'steamwebhelper|steam$|steam -' >/dev/null 2>&1; then
      return 0
    fi
    sleep 1
  done
  return 1
}

steam_is_running() {
  pgrep -u "$USER" -f 'steamwebhelper|steam$|steam -' >/dev/null 2>&1
}

should_stop_steam() {
  case "$STOP_STEAM" in
    1) return 0 ;;
    0) return 1 ;;
    auto) steam_is_running ;;
  esac
  return 1
}

if should_stop_steam; then
  log "stopping SteamVR/Steam so Vulkan/env is applied cleanly (XR_OPENVR_STOP_STEAM=$STOP_STEAM)"
  stop_steamvr_processes
  steam -shutdown >/dev/null 2>&1 || true
  if ! wait_for_steam_shutdown; then
    log "warning: Steam still appears to be running; launch env may not propagate to SteamVR"
  fi
else
  stop_steamvr_processes
  if [[ "$STOP_STEAM" == "auto" ]]; then
    log "Steam is not running; skipping steam -shutdown"
  else
    log "XR_OPENVR_STOP_STEAM=0; skipping steam -shutdown"
  fi
fi

if [[ "$SET_DISPLAY" == "1" ]]; then
  log "preparing dGPU direct display: output=$XR_OUT mode=$XR_MODE rate=${XR_RATE}Hz"
  if ! xrandr --query | awk '/ connected| disconnected/ {print $1}' | grep -qx -- "$XR_OUT"; then
    xrandr --query | grep -E ' connected| disconnected' >&2 || true
    fatal "XREAL output not found: $XR_OUT; override with XR_OPENVR_DGPU_OUTPUT=<output>"
  fi
  xrandr --output "$XR_OUT" --set non-desktop 1 || true
  # Direct mode works best when the HMD output is not part of the normal desktop.
  xrandr --output "$XR_OUT" --off || true
fi

if [[ "$CLEAR_LOGS" == "1" ]]; then
  log "clearing OpenVR logs"
  rm -rf "$HOME/.config/openvr/logs"
  mkdir -p "$HOME/.config/openvr/logs"
fi

log "registering OpenVR driver variant: ${OPENVR_FREQ}Hz ${OPENVR_MODE} (${DRIVER_DIR_NAME})"
XR_PACKAGE_ROOT="$PACKAGE_ROOT" \
XR_ROOT_PROJECT="$PACKAGE_ROOT" \
ROOT_PROJECT="$PACKAGE_ROOT" \
DRIVERS_ROOT="$DRIVERS_ROOT" \
XR_OPENVR_DEVICE="$OPENVR_DEVICE" \
XR_TARGET_DEVICE="$OPENVR_DEVICE" \
XR_DEVICE_TARGET="$OPENVR_DEVICE" \
XR_DISPLAY_FREQUENCY_HZ="$OPENVR_FREQ" \
XR_OPENVR_DISPLAY_FREQUENCY_HZ="$OPENVR_FREQ" \
XR_OPENVR_DISPLAY_MODE="$OPENVR_MODE" \
XR_OPENVR_DRIVER_DIR_NAME="$DRIVER_DIR_NAME" \
XR_OPENVR_REGISTER_METHOD="$REGISTER_METHOD" \
XR_OPENVR_RUNTIME_MODE="$RUNTIME_MODE" \
  "$REGISTER_SCRIPT"

GPU_ICD="$(resolve_gpu_icd "$GPU_VENDOR" || true)"
if [[ -z "$GPU_ICD" ]]; then
  case "$GPU_VENDOR" in
    nvidia)
      fatal "NVIDIA Vulkan ICD not found; set VK_ICD_FILENAMES=/path/to/nvidia_icd.json or use XR_OPENVR_DGPU_VENDOR=amd"
      ;;
    amd)
      fatal "AMD Vulkan ICD not found; install Mesa Vulkan drivers or set VK_ICD_FILENAMES=/path/to/radeon_icd.x86_64.json"
      ;;
    auto)
      fatal "Vulkan ICD not found for NVIDIA or AMD; set VK_ICD_FILENAMES=/path/to/icd.json"
      ;;
  esac
fi

log "runtime_root=$RUNTIME_ROOT"
log "package_root=$PACKAGE_ROOT"
log "drivers_root=$DRIVERS_ROOT"
log "register_script=$REGISTER_SCRIPT"
log "register_method=$REGISTER_METHOD"
log "runtime_mode=$RUNTIME_MODE"
log "gpu_vendor=$GPU_VENDOR"
log "stop_steam=$STOP_STEAM"
log "VK_ICD_FILENAMES=$GPU_ICD"
if [[ "$GPU_VENDOR" == "amd" || "$GPU_VENDOR" == "auto" ]]; then
  log "AMD_DRI_PRIME=${AMD_DRI_PRIME:-<unset>}"
fi
log "XR_OUT=$XR_OUT"
log "driver=$DRIVER_DIR_NAME"
log "frequency=${OPENVR_FREQ}Hz"
log "device=$OPENVR_DEVICE"
log "launch_mode=$LAUNCH_MODE"

COMMON_ENV=(
  VK_ICD_FILENAMES="$GPU_ICD"
  XR_PACKAGE_ROOT="$PACKAGE_ROOT"
  XR_ROOT_PROJECT="$PACKAGE_ROOT"
  ROOT_PROJECT="$PACKAGE_ROOT"
  DRIVERS_ROOT="$DRIVERS_ROOT"
  XR_OPENVR_DGPU_OUTPUT="$XR_OUT"
  XR_OPENVR_DISPLAY_FREQUENCY_HZ="$OPENVR_FREQ"
  XR_DISPLAY_FREQUENCY_HZ="$OPENVR_FREQ"
  XR_OPENVR_DISPLAY_MODE="$OPENVR_MODE"
  XR_OPENVR_DEVICE="$OPENVR_DEVICE"
  XR_TARGET_DEVICE="$OPENVR_DEVICE"
  XR_OPENVR_DRIVER_DIR_NAME="$DRIVER_DIR_NAME"
  XR_OPENVR_REGISTER_METHOD="$REGISTER_METHOD"
  XR_OPENVR_RUNTIME_MODE="$RUNTIME_MODE"
  XR_OPENVR_DGPU_VENDOR="$GPU_VENDOR"
)

case "$GPU_VENDOR" in
  nvidia)
    COMMON_ENV+=(
      __GLX_VENDOR_LIBRARY_NAME=nvidia
      __NV_PRIME_RENDER_OFFLOAD=0
    )
    ;;
  amd|auto)
    if [[ -n "$AMD_DRI_PRIME" ]]; then
      COMMON_ENV+=(DRI_PRIME="$AMD_DRI_PRIME")
    fi
    ;;
esac

case "$LAUNCH_MODE" in
  none)
    log "configured only; launch skipped"
    exit 0
    ;;
  steam)
    log "starting Steam client only; SteamVR is not launched now"
    exec env "${COMMON_ENV[@]}" steam
    ;;
  steamvr)
    log "starting SteamVR"
    exec env "${COMMON_ENV[@]}" steam steam://rungameid/250820
    ;;
esac