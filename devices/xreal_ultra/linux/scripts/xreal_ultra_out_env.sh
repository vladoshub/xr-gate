#!/usr/bin/env bash
# XREAL Ultra naming/profile adapter for the common device output environment.

_xr_xreal_env_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export XR_TARGET_DEVICE="${XR_TARGET_DEVICE:-xreal_ultra}"
export XR_DEVICE_TARGET="${XR_DEVICE_TARGET:-$XR_TARGET_DEVICE}"
export XR_DEVICE_ENV_NAME="${XR_DEVICE_ENV_NAME:-xreal_ultra.env}"
export XR_DEVICE_DISPLAY_NAME="${XR_DEVICE_DISPLAY_NAME:-XREAL Ultra}"
export XR_OUT_PACKAGE_NAME="${XR_OUT_PACKAGE_NAME:-xreal_ultra}"
export XR_DEVICE_OUT_ENV="${XR_DEVICE_OUT_ENV:-$_xr_xreal_env_dir/xreal_ultra_out_env.sh}"

_xr_common_out_env="$_xr_xreal_env_dir/../../../common/linux/scripts/build/device_out_env.sh"
if [[ ! -f "$_xr_common_out_env" ]]; then
  echo "[xreal_ultra_out_env][ERROR] common output environment not found: $_xr_common_out_env" >&2
  return 2 2>/dev/null || exit 2
fi
# shellcheck source=/dev/null
source "$_xr_common_out_env"
unset _xr_common_out_env _xr_xreal_env_dir
