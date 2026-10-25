#!/usr/bin/env bash
# Build one package per configured device; hardware values stay in profiles.
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_PROJECT="${XR_ROOT_PROJECT:-${ROOT_PROJECT:-$(cd "$SCRIPT_DIR/.." && pwd)}}"
CATALOG="$ROOT_PROJECT/drivers/device_profiles.py"
BIN_ROOT="${XR_BIN_ROOT:-$ROOT_PROJECT/bin}"
BUILD_ROOT="${XR_BUILD_ROOT:-$ROOT_PROJECT/build}"
driver="${1:?Usage: build_profiles.sh steamvr|monado}"
case "$driver" in
  steamvr) requested="${XR_OPENVR_DEVICES:-${XR_DRIVER_DEVICES:-${XR_OPENVR_DEVICE:-}}}" ;;
  monado) requested="${XR_MONADO_DEVICES:-${XR_DRIVER_DEVICES:-${XR_MONADO_DEVICE:-}}}" ;;
  *) echo "[drivers] Unknown driver: $driver" >&2; exit 2 ;;
esac
# Capture the exit status before mapfile: process substitutions hide failures.
configured="$(python3 "$CATALOG" --root "$ROOT_PROJECT" --driver "$driver" --devices "$requested")"
mapfile -t devices <<<"$configured"
if [[ "$driver" == steamvr ]]; then
  source "$ROOT_PROJECT/drivers/openvr_driver/scripts/openvr_display_common.sh"
  drivers_root="${INSTALL_DRIVERS_ROOT:-$BIN_ROOT/drivers}"
  frequencies="${XR_OPENVR_BUILD_FREQUENCIES:-60 75 90}"
  modes="${XR_OPENVR_BUILD_MODES:-direct}"
  tag="$(openvr_normalize_package_tag "${XR_OPENVR_PACKAGE_TAG:-}")"
  for device in "${devices[@]}"; do
    for raw_mode in $modes; do
      mode="$(openvr_normalize_display_mode "$raw_mode")"
      for raw_frequency in $frequencies; do
        frequency="$(openvr_normalize_display_frequency_hz "$raw_frequency")"
        name="$(openvr_driver_dir_name "$frequency" "$mode" "$device" "$tag")"
        echo "[drivers] SteamVR device=$device frequency=$frequency mode=$mode" >&2
        XR_OPENVR_DEVICE="$device" \
        XR_OPENVR_DISPLAY_FREQUENCY_HZ="$frequency" XR_OPENVR_DISPLAY_MODE="$mode" \
        XR_OPENVR_DRIVER_DIR_NAME="$name" XR_OPENVR_SINGLE_VARIANT_BUILD=1 \
        INSTALL_DRIVERS_ROOT="$drivers_root" \
        INSTALL_DRIVER_ROOT="$drivers_root/$device/$name" \
        INSTALL_DRIVER_PACKAGE="$drivers_root/$device/$name/xr_tracking" \
        BUILD_DIR="${OPENVR_BUILD_ROOT:-$BUILD_ROOT/drivers}/$device/$name" \
        CLONE_OPENVR="${CLONE_OPENVR:-1}" \
          bash "$ROOT_PROJECT/drivers/openvr_driver/scripts/build_driver.sh"
      done
    done
  done
else
  # Reuse the hardware-independent CMake build; install per-device settings.
  for device in "${devices[@]}"; do
    echo "[drivers] Monado device=$device" >&2
    XR_MONADO_DEVICE="$device" XR_TARGET_DEVICE="$device" XR_DEVICE_TARGET="$device" \
    BIN_DIR="$BIN_ROOT/drivers/$device/monado_driver" XR_BIN_ROOT="$BIN_ROOT" \
    BUILD_DIR="${MONADO_BUILD_DIR:-$BUILD_ROOT/monado_driver}" \
    CLONE_MONADO="${CLONE_MONADO:-1}" \
      bash "$ROOT_PROJECT/drivers/monado_driver/scripts/linux/install.sh"
  done
fi
