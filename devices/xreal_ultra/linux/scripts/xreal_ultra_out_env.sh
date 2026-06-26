#!/usr/bin/env bash
# Shared build/package output environment for XREAL Ultra.
# Source this file from build/install/package scripts to keep all runtime files
# under one relocatable output tree.

_xr_env_script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export ROOT_PROJECT="${ROOT_PROJECT:-$(cd "$_xr_env_script_dir/../../../.." && pwd)}"
export XR_ROOT_PROJECT="${XR_ROOT_PROJECT:-$ROOT_PROJECT}"

# Global output root for deployable artifacts. Override once and all package/build
# wrappers will use the same destination.
export XR_OUT_ROOT="${XR_OUT_ROOT:-$XR_ROOT_PROJECT/out/xreal_ultra}"
export XR_OUT_BIN_ROOT="${XR_OUT_BIN_ROOT:-$XR_OUT_ROOT/bin}"
export XR_OUT_DEVICE_HOME="${XR_OUT_DEVICE_HOME:-$XR_OUT_ROOT/devices/xreal_ultra}"
export XR_OUT_SCRIPTS_ROOT="${XR_OUT_SCRIPTS_ROOT:-$XR_OUT_DEVICE_HOME/linux/scripts}"
export XR_OUT_CONFIGS_ROOT="${XR_OUT_CONFIGS_ROOT:-$XR_OUT_DEVICE_HOME/configs}"

# When building directly into the package, use this as XR_BIN_ROOT.
export XR_BIN_ROOT="${XR_BIN_ROOT:-$XR_OUT_BIN_ROOT}"

# Optional source of already installed binaries. package_xreal_ultra_out.sh copies
# from this root into XR_OUT_BIN_ROOT. Defaults to the project-local bin/ tree.
export XR_PACKAGE_SOURCE_BIN_ROOT="${XR_PACKAGE_SOURCE_BIN_ROOT:-$XR_ROOT_PROJECT/bin}"
