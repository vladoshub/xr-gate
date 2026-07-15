#!/usr/bin/env bash
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=/dev/null
source "$SCRIPT_DIR/../lib/device_runtime.sh"
xr_load_device_env

log() { echo "[device/openvr_register] $*" >&2; }

candidate_scripts=()
add_candidate() { [[ -n "$1" ]] && candidate_scripts+=("$1"); }
add_candidate "${OPENVR_REGISTER_DRIVER_SCRIPT:-}"
if [[ -n "${XR_OUT_ROOT:-}" && -n "${XR_OPENVR_DRIVER_DIR_NAME:-}" ]]; then
  add_candidate "$XR_OUT_ROOT/bin/drivers/$XR_OPENVR_DRIVER_DIR_NAME/scripts/register_driver.sh"
fi
if [[ -n "${XR_OUT_ROOT:-}" ]]; then
  add_candidate "$XR_OUT_ROOT/bin/drivers/openvr_driver/scripts/register_driver.sh"
fi
add_candidate "$XR_ROOT_PROJECT/drivers/openvr_driver/scripts/register_driver.sh"
add_candidate "$XR_BIN_ROOT/scripts/drivers/openvr_driver/register_driver.sh"

seen=()
for script in "${candidate_scripts[@]}"; do
  duplicate=0
  for prev in "${seen[@]}"; do [[ "$prev" == "$script" ]] && duplicate=1 && break; done
  [[ "$duplicate" == 0 ]] || continue
  seen+=("$script")
  [[ -x "$script" ]] || continue
  log "using register script: $script"
  case "$script" in
    "${XR_OUT_ROOT:-__no_out_root__}"/bin/drivers/*/scripts/register_driver.sh)
      exec env XR_PACKAGE_ROOT="$XR_OUT_ROOT" XR_ROOT_PROJECT="$XR_OUT_ROOT" \
        ROOT_PROJECT="$XR_OUT_ROOT" XR_BIN_ROOT="$XR_OUT_ROOT/bin" "$script" "$@"
      ;;
    "${XR_ROOT_PROJECT:-__no_root_project__}"/drivers/openvr_driver/scripts/register_driver.sh)
      if [[ -n "${XR_OUT_ROOT:-}" && -d "$XR_OUT_ROOT/bin/drivers" ]]; then
        exec env DRIVERS_ROOT="$XR_OUT_ROOT/bin/drivers" "$script" "$@"
      fi
      exec "$script" "$@"
      ;;
    *) exec "$script" "$@" ;;
  esac
done
xr_common_fatal "OpenVR register script not found. Tried: ${seen[*]}"
