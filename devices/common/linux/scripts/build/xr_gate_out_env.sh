#!/usr/bin/env bash
# Generic XR Gate build/package environment.
#
# The output package is hardware-neutral. Runtime hardware selection is made by
# xr_client --config, while all supported runtime profiles are packaged together.

_xr_gate_env_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export XR_TARGET_DEVICE="${XR_TARGET_DEVICE:-generic}"
export XR_DEVICE_TARGET="${XR_DEVICE_TARGET:-$XR_TARGET_DEVICE}"
export XR_DEVICE_DISPLAY_NAME="${XR_DEVICE_DISPLAY_NAME:-XR Gate}"
export XR_OUT_PACKAGE_NAME="${XR_OUT_PACKAGE_NAME:-xr-gate}"
# One profile list drives the packaged xr_client configs and device/tracking
# bundles by default. The lower-level lists remain independently overridable.
export XR_PACKAGE_PROFILES="${XR_PACKAGE_PROFILES:-xreal_ultra leap_motion_uvc_nrf54l15}"
export XR_PACKAGE_DEVICE_TARGETS="${XR_PACKAGE_DEVICE_TARGETS:-$XR_PACKAGE_PROFILES}"
export XR_PACKAGE_CONFIG_PROFILES="${XR_PACKAGE_CONFIG_PROFILES:-$XR_PACKAGE_PROFILES}"
export XR_VENDOR_COMPONENTS="${XR_VENDOR_COMPONENTS:-xreal_ultra}"
# Paths are relative to XR_OUT_ROOT and are deleted when vendor packaging is off,
# preventing stale binaries from a previous local build from leaking in.
export XR_VENDOR_PACKAGE_PATHS="${XR_VENDOR_PACKAGE_PATHS:-bin/xreal_display_helper bin/scripts/drivers/steam_vr/restore_xreal_desktop.sh run_openvr_restore_desktop.sh}"
export XR_BUILD_VENDOR_COMPONENTS="${XR_BUILD_VENDOR_COMPONENTS:-1}"
export XR_PACKAGE_VENDOR_COMPONENTS="${XR_PACKAGE_VENDOR_COMPONENTS:-$XR_BUILD_VENDOR_COMPONENTS}"
export XR_DEVICE_BUILD_HOOK="${XR_DEVICE_BUILD_HOOK:-$_xr_gate_env_dir/build_vendor_components.sh}"
export XR_DEVICE_PACKAGE_HOOK="${XR_DEVICE_PACKAGE_HOOK:-$_xr_gate_env_dir/package_vendor_components.sh}"
export XR_DEVICE_OUT_ENV="${XR_DEVICE_OUT_ENV:-$_xr_gate_env_dir/xr_gate_out_env.sh}"

# shellcheck source=/dev/null
source "$_xr_gate_env_dir/device_out_env.sh"
unset _xr_gate_env_dir
