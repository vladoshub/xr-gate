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

normalize_display_frequency_hz() {
  local value="${1:-60}"
  python3 - "$value" <<'PY'
import math
import sys
text = sys.argv[1].strip()
try:
    value = float(text)
except ValueError:
    print(f"[ERROR] Unsupported display frequency: {text}", file=sys.stderr)
    print("Expected integer Hz in range 60..120.", file=sys.stderr)
    sys.exit(2)
if not math.isfinite(value) or abs(value - round(value)) > 1e-6:
    print(f"[ERROR] Unsupported display frequency: {text}", file=sys.stderr)
    print("Expected integer Hz in range 60..120.", file=sys.stderr)
    sys.exit(2)
hz = int(round(value))
if hz < 60 or hz > 120:
    print(f"[ERROR] Unsupported display frequency: {text}", file=sys.stderr)
    print("Expected integer Hz in range 60..120.", file=sys.stderr)
    sys.exit(2)
print(hz)
PY
}

normalize_display_mode() {
  local value="${1:-direct}"
  value="${value,,}"
  value="${value//-/_}"
  case "$value" in
    direct|direct_mode|drm|drm_lease) printf 'direct\n' ;;
    extended|extended_sbs|sbs|windowed|desktop) printf 'extended_sbs\n' ;;
    *)
      echo "[ERROR] Unsupported OpenVR display mode: $1" >&2
      echo "Expected direct or extended_sbs." >&2
      exit 2
      ;;
  esac
}

openvr_driver_dir_name() {
  local hz="$1"
  local mode="$2"
  if [[ "$mode" == "direct" ]]; then
    printf 'openvr_driver_%sHZ\n' "$hz"
  else
    printf 'openvr_driver_%sHZ_%s\n' "$hz" "$mode"
  fi
}

append_unique() {
  local value="$1"
  shift
  local item
  for item in "$@"; do
    if [[ "$item" == "$value" ]]; then
      printf '%s\n' "$@"
      return 0
    fi
  done
  printf '%s\n' "$@" "$value"
}

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DRIVER_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
PROJECT_ROOT="$(cd "${DRIVER_ROOT}/../.." && pwd)"

SELECTED_DISPLAY_FREQUENCY_HZ_RAW="${XR_OPENVR_DISPLAY_FREQUENCY_HZ:-${XR_DISPLAY_FREQUENCY_HZ:-${DISPLAY_FREQUENCY_HZ:-60}}}"
SELECTED_DISPLAY_FREQUENCY_HZ="$(normalize_display_frequency_hz "$SELECTED_DISPLAY_FREQUENCY_HZ_RAW")"
SELECTED_DISPLAY_MODE="$(normalize_display_mode "${XR_OPENVR_DISPLAY_MODE:-${XR_STEAMVR_DISPLAY_MODE:-direct}}")"
XR_OPENVR_DEVICE_RAW="${XR_OPENVR_DEVICE:-${XR_DEVICE_TARGET:-${XR_TARGET_DEVICE:-generic}}}"
XR_OPENVR_DEVICE="${XR_OPENVR_DEVICE_RAW,,}"
XR_OPENVR_DEVICE="${XR_OPENVR_DEVICE//-/_}"
case "$XR_OPENVR_DEVICE" in
  generic|none) XR_OPENVR_DEVICE="generic" ;;
  xreal_ultra|xreal_air2ultra) XR_OPENVR_DEVICE="xreal_ultra" ;;
  *)
    echo "[ERROR] Unsupported OpenVR device profile: $XR_OPENVR_DEVICE_RAW" >&2
    echo "Expected generic or xreal_ultra." >&2
    exit 2
    ;;
esac

BUILD_ROOT="${BUILD_ROOT:-$PROJECT_ROOT/build/drivers}"
BUILD_ROOT="$(expand_tilde "$BUILD_ROOT")"

INSTALL_DRIVERS_ROOT="${INSTALL_DRIVERS_ROOT:-$PROJECT_ROOT/bin/drivers}"
INSTALL_DRIVERS_ROOT="$(expand_tilde "$INSTALL_DRIVERS_ROOT")"

readarray -t BUILD_FREQUENCIES < <(
  for raw in ${XR_OPENVR_BUILD_FREQUENCIES:-60 75 90}; do
    normalize_display_frequency_hz "$raw"
  done | awk '!seen[$0]++'
)
readarray -t BUILD_FREQUENCIES < <(append_unique "$SELECTED_DISPLAY_FREQUENCY_HZ" "${BUILD_FREQUENCIES[@]}")

readarray -t BUILD_MODES < <(
  for raw in ${XR_OPENVR_BUILD_MODES:-direct}; do
    normalize_display_mode "$raw"
  done | awk '!seen[$0]++'
)
readarray -t BUILD_MODES < <(append_unique "$SELECTED_DISPLAY_MODE" "${BUILD_MODES[@]}")

printf '[xr] Building OpenVR driver variants\n'
printf '[xr] PROJECT_ROOT=%s\n' "$PROJECT_ROOT"
printf '[xr] DRIVER_ROOT=%s\n' "$DRIVER_ROOT"
printf '[xr] BUILD_ROOT=%s\n' "$BUILD_ROOT"
printf '[xr] INSTALL_DRIVERS_ROOT=%s\n' "$INSTALL_DRIVERS_ROOT"
printf '[xr] REGISTER_DISPLAY_FREQUENCY_HZ=%s\n' "$SELECTED_DISPLAY_FREQUENCY_HZ"
printf '[xr] REGISTER_DISPLAY_MODE=%s\n' "$SELECTED_DISPLAY_MODE"
printf '[xr] DEVICE=%s\n' "$XR_OPENVR_DEVICE"
printf '[xr] BUILD_FREQUENCIES=%s\n' "${BUILD_FREQUENCIES[*]}"
printf '[xr] BUILD_MODES=%s\n' "${BUILD_MODES[*]}"

for mode in "${BUILD_MODES[@]}"; do
  for freq in "${BUILD_FREQUENCIES[@]}"; do
    driver_dir_name="$(openvr_driver_dir_name "$freq" "$mode")"
    install_driver_root="$INSTALL_DRIVERS_ROOT/$driver_dir_name"
    build_dir="$BUILD_ROOT/$driver_dir_name"

    echo
    printf '[xr] Building OpenVR driver package: %s\n' "$driver_dir_name"
    XR_OPENVR_DEVICE="$XR_OPENVR_DEVICE" \
    XR_OPENVR_DISPLAY_FREQUENCY_HZ="$freq" \
    XR_OPENVR_DISPLAY_MODE="$mode" \
    XR_OPENVR_DRIVER_DIR_NAME="$driver_dir_name" \
    XR_OPENVR_SINGLE_VARIANT_BUILD=1 \
    BUILD_DIR="$build_dir" \
    INSTALL_DRIVER_ROOT="$install_driver_root" \
      "${SCRIPT_DIR}/build_driver.sh"

    driver_so="$install_driver_root/xr_tracking/bin/linux64/driver_xr_tracking.so"
    if [[ ! -f "$driver_so" ]]; then
      echo "ERROR: built driver not found: $driver_so" >&2
      exit 1
    fi
  done
done

selected_driver_dir_name="$(openvr_driver_dir_name "$SELECTED_DISPLAY_FREQUENCY_HZ" "$SELECTED_DISPLAY_MODE")"
selected_driver_package="$INSTALL_DRIVERS_ROOT/$selected_driver_dir_name/xr_tracking"

if [[ "${XR_OPENVR_REGISTER_AFTER_BUILD:-1}" == "1" ]]; then
  printf '\n[xr] Registering one SteamVR driver variant\n'
  printf '[xr] DRIVER_PACKAGE=%s\n' "$selected_driver_package"
  printf '[xr] DISPLAY_FREQUENCY_HZ=%s\n' "$SELECTED_DISPLAY_FREQUENCY_HZ"
  printf '[xr] DISPLAY_MODE=%s\n' "$SELECTED_DISPLAY_MODE"

  XR_OPENVR_DEVICE="$XR_OPENVR_DEVICE" \
  XR_OPENVR_DISPLAY_FREQUENCY_HZ="$SELECTED_DISPLAY_FREQUENCY_HZ" \
  XR_OPENVR_DISPLAY_MODE="$SELECTED_DISPLAY_MODE" \
  XR_OPENVR_DRIVER_DIR_NAME="$selected_driver_dir_name" \
  DRIVER_PACKAGE="$selected_driver_package" \
    "${SCRIPT_DIR}/register_driver.sh"

  echo
  echo "[xr] OK: OpenVR packages built; registered ${SELECTED_DISPLAY_FREQUENCY_HZ}Hz ${SELECTED_DISPLAY_MODE} only"
else
  echo
  echo "[xr] OK: OpenVR packages built; registration skipped (XR_OPENVR_REGISTER_AFTER_BUILD=0)"
fi
