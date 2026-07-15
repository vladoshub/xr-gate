#!/usr/bin/env bash
# Shared build/package output environment for an XR Gate device target.
#
# Device wrappers should set XR_TARGET_DEVICE and any naming overrides before
# sourcing this file. The defaults intentionally work for a conventional
# devices/<target>/<target>.env layout.

_xr_common_out_env_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export ROOT_PROJECT="${ROOT_PROJECT:-$(cd "$_xr_common_out_env_dir/../../../../.." && pwd)}"
export XR_ROOT_PROJECT="${XR_ROOT_PROJECT:-$ROOT_PROJECT}"

export XR_TARGET_DEVICE="${XR_TARGET_DEVICE:-generic}"
export XR_DEVICE_TARGET="${XR_DEVICE_TARGET:-$XR_TARGET_DEVICE}"
export XR_DEVICE_ENV_NAME="${XR_DEVICE_ENV_NAME:-$XR_TARGET_DEVICE.env}"
export XR_DEVICE_DISPLAY_NAME="${XR_DEVICE_DISPLAY_NAME:-$XR_TARGET_DEVICE}"
export XR_OUT_PACKAGE_NAME="${XR_OUT_PACKAGE_NAME:-$XR_TARGET_DEVICE}"

export XR_DEVICE_SOURCE_HOME="${XR_DEVICE_SOURCE_HOME:-$XR_ROOT_PROJECT/devices/$XR_TARGET_DEVICE}"
export XR_COMMON_SOURCE_HOME="${XR_COMMON_SOURCE_HOME:-$XR_ROOT_PROJECT/devices/common}"

# Global output root for deployable artifacts. Override once and all package/build
# wrappers use the same destination.
export XR_OUT_ROOT="${XR_OUT_ROOT:-$XR_ROOT_PROJECT/out/$XR_OUT_PACKAGE_NAME}"
export XR_OUT_BIN_ROOT="${XR_OUT_BIN_ROOT:-$XR_OUT_ROOT/bin}"
export XR_OUT_DEVICE_HOME="${XR_OUT_DEVICE_HOME:-$XR_OUT_ROOT/devices/$XR_TARGET_DEVICE}"
export XR_OUT_COMMON_HOME="${XR_OUT_COMMON_HOME:-$XR_OUT_ROOT/devices/common}"
export XR_OUT_SCRIPTS_ROOT="${XR_OUT_SCRIPTS_ROOT:-$XR_OUT_DEVICE_HOME/linux/scripts}"
export XR_OUT_CONFIGS_ROOT="${XR_OUT_CONFIGS_ROOT:-$XR_OUT_DEVICE_HOME/configs}"

# When building directly into the package, use this as XR_BIN_ROOT.
export XR_BIN_ROOT="${XR_BIN_ROOT:-$XR_OUT_BIN_ROOT}"

# Optional source of already installed binaries. package_device_out.sh copies
# from this root into XR_OUT_BIN_ROOT. Defaults to the project-local bin/ tree.
export XR_PACKAGE_SOURCE_BIN_ROOT="${XR_PACKAGE_SOURCE_BIN_ROOT:-$XR_ROOT_PROJECT/bin}"

unset _xr_common_out_env_dir
