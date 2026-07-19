#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=/dev/null
source "$SCRIPT_DIR/../lib/device_runtime.sh"

# override_controller is hardware-neutral. When a device profile is already
# selected, keep loading it for backward compatibility and provider defaults.
# For standalone use, fall back to the shared package environment instead of
# requiring XR_DEVICE_ENV/XR_DEVICE_HOME.
if xr_resolve_device_env >/dev/null 2>&1; then
  xr_load_device_env
else
  export XR_ROOT_PROJECT="${XR_ROOT_PROJECT:-$(xr_common_repo_root)}"
  export ROOT_PROJECT="${ROOT_PROJECT:-$XR_ROOT_PROJECT}"
  export XR_PACKAGE_ROOT="${XR_PACKAGE_ROOT:-$XR_ROOT_PROJECT}"
  export XR_DEVICE_HOME="${XR_DEVICE_HOME:-$XR_ROOT_PROJECT/devices/common}"

  COMMON_ENV="$XR_ROOT_PROJECT/devices/common/common.env"
  if [[ ! -f "$COMMON_ENV" ]]; then
    xr_common_fatal "common runtime env not found: $COMMON_ENV"
  fi
  # shellcheck source=/dev/null
  source "$COMMON_ENV"

  # In a source checkout this enables AUTO_BUILD when explicitly requested.
  # A packaged release normally uses AUTO_BUILD=0 and only needs the binary.
  if [[ ! -d "${OVERRIDE_CONTROLLER_DIR:-}" && -d "$XR_ROOT_PROJECT/override_controller" ]]; then
    export OVERRIDE_CONTROLLER_DIR="$XR_ROOT_PROJECT/override_controller"
  fi

  xr_common_log "no device env selected; using hardware-neutral common environment"
fi

xr_exec_runtime_script override_controller \
  scripts/override_controller/start_override_controller.sh \
  override_controller/scripts/linux/start_override_controller.sh \
  "$@"
