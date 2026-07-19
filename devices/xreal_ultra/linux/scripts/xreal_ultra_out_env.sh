#!/usr/bin/env bash
# Compatibility adapter. Build/package output is now generic and contains all
# configured runtime profiles; XREAL is selected only by xr_client config.
_xr_xreal_env_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
_xr_root="${XR_ROOT_PROJECT:-$(cd "$_xr_xreal_env_dir/../../../.." && pwd)}"
# shellcheck source=/dev/null
source "$_xr_root/devices/common/linux/scripts/build/xr_gate_out_env.sh"
unset _xr_root _xr_xreal_env_dir
