#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=./openvr_display_common.sh
source "$SCRIPT_DIR/openvr_display_common.sh"

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

DRIVER_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
PROJECT_ROOT="$(cd "${DRIVER_ROOT}/../.." && pwd)"

SELECTED_DISPLAY_FREQUENCY_HZ_RAW="${XR_OPENVR_DISPLAY_FREQUENCY_HZ:-${XR_DISPLAY_FREQUENCY_HZ:-${DISPLAY_FREQUENCY_HZ:-60}}}"
SELECTED_DISPLAY_FREQUENCY_HZ="$(openvr_normalize_display_frequency_hz "$SELECTED_DISPLAY_FREQUENCY_HZ_RAW")"
SELECTED_DISPLAY_MODE="$(openvr_normalize_display_mode "${XR_OPENVR_DISPLAY_MODE:-${XR_STEAMVR_DISPLAY_MODE:-direct}}")"
XR_OPENVR_DEVICE="$(openvr_normalize_profile_name "${XR_OPENVR_DEVICE:-${XR_DEVICE_TARGET:-${XR_TARGET_DEVICE:-generic}}}")"
XR_OPENVR_PACKAGE_TAG="$(openvr_normalize_package_tag "${XR_OPENVR_PACKAGE_TAG:-}")"
XR_OPENVR_DEVICE_SETTINGS="$(openvr_resolve_device_settings "$DRIVER_ROOT" "$XR_OPENVR_DEVICE" "${XR_OPENVR_DEVICE_SETTINGS:-}")"

BUILD_ROOT="${BUILD_ROOT:-$PROJECT_ROOT/build/drivers}"
BUILD_ROOT="$(openvr_expand_tilde "$BUILD_ROOT")"

INSTALL_DRIVERS_ROOT="${INSTALL_DRIVERS_ROOT:-$PROJECT_ROOT/bin/drivers}"
INSTALL_DRIVERS_ROOT="$(openvr_expand_tilde "$INSTALL_DRIVERS_ROOT")"

readarray -t BUILD_FREQUENCIES < <(
  for raw in ${XR_OPENVR_BUILD_FREQUENCIES:-60 75 90}; do
    openvr_normalize_display_frequency_hz "$raw"
  done | awk '!seen[$0]++'
)
readarray -t BUILD_FREQUENCIES < <(append_unique "$SELECTED_DISPLAY_FREQUENCY_HZ" "${BUILD_FREQUENCIES[@]}")

readarray -t BUILD_MODES < <(
  for raw in ${XR_OPENVR_BUILD_MODES:-direct}; do
    openvr_normalize_display_mode "$raw"
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
printf '[xr] DEVICE_SETTINGS=%s\n' "${XR_OPENVR_DEVICE_SETTINGS:-<none>}"
printf '[xr] PACKAGE_TAG=%s\n' "${XR_OPENVR_PACKAGE_TAG:-<none>}"
printf '[xr] BUILD_FREQUENCIES=%s\n' "${BUILD_FREQUENCIES[*]}"
printf '[xr] BUILD_MODES=%s\n' "${BUILD_MODES[*]}"

for mode in "${BUILD_MODES[@]}"; do
  for freq in "${BUILD_FREQUENCIES[@]}"; do
    driver_dir_name="$(openvr_driver_dir_name "$freq" "$mode" "$XR_OPENVR_DEVICE" "$XR_OPENVR_PACKAGE_TAG")"
    install_driver_root="$INSTALL_DRIVERS_ROOT/$driver_dir_name"
    build_dir="$BUILD_ROOT/$driver_dir_name"

    echo
    printf '[xr] Building OpenVR driver package: %s\n' "$driver_dir_name"
    XR_OPENVR_DEVICE="$XR_OPENVR_DEVICE" \
    XR_OPENVR_DEVICE_SETTINGS="$XR_OPENVR_DEVICE_SETTINGS" \
    XR_OPENVR_PACKAGE_TAG="$XR_OPENVR_PACKAGE_TAG" \
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

selected_driver_dir_name="$(openvr_driver_dir_name "$SELECTED_DISPLAY_FREQUENCY_HZ" "$SELECTED_DISPLAY_MODE" "$XR_OPENVR_DEVICE" "$XR_OPENVR_PACKAGE_TAG")"
selected_driver_package="$INSTALL_DRIVERS_ROOT/$selected_driver_dir_name/xr_tracking"

if [[ "${XR_OPENVR_REGISTER_AFTER_BUILD:-1}" == "1" ]]; then
  printf '\n[xr] Registering one SteamVR driver variant\n'
  printf '[xr] DRIVER_PACKAGE=%s\n' "$selected_driver_package"
  printf '[xr] DISPLAY_FREQUENCY_HZ=%s\n' "$SELECTED_DISPLAY_FREQUENCY_HZ"
  printf '[xr] DISPLAY_MODE=%s\n' "$SELECTED_DISPLAY_MODE"
  printf '[xr] DEVICE=%s\n' "$XR_OPENVR_DEVICE"

  XR_OPENVR_DEVICE="$XR_OPENVR_DEVICE" \
  XR_OPENVR_PACKAGE_TAG="$XR_OPENVR_PACKAGE_TAG" \
  XR_OPENVR_DISPLAY_FREQUENCY_HZ="$SELECTED_DISPLAY_FREQUENCY_HZ" \
  XR_OPENVR_DISPLAY_MODE="$SELECTED_DISPLAY_MODE" \
  XR_OPENVR_DRIVER_DIR_NAME="$selected_driver_dir_name" \
  DRIVER_PACKAGE="$selected_driver_package" \
    "${SCRIPT_DIR}/register_driver.sh"

  echo
  echo "[xr] OK: OpenVR packages built; registered ${SELECTED_DISPLAY_FREQUENCY_HZ}Hz ${SELECTED_DISPLAY_MODE} profile=${XR_OPENVR_DEVICE} only"
else
  echo
  echo "[xr] OK: OpenVR packages built; registration skipped (XR_OPENVR_REGISTER_AFTER_BUILD=0)"
fi
