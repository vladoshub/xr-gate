#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEVICE_HOME="$(cd "$SCRIPT_DIR/../../.." && pwd)"
# shellcheck source=/dev/null
source "$DEVICE_HOME/xreal_ultra.env"

log() { echo "[xreal_openvr_register] $*" >&2; }
fatal() { echo "[xreal_openvr_register][ERROR] $*" >&2; exit 1; }

# Device wrappers may be run either from the source tree:
#   <repo>/devices/xreal_ultra/linux/scripts/openvr_driver/register_driver.sh
# or from a packaged output tree:
#   <package>/devices/xreal_ultra/linux/scripts/openvr_driver/register_driver.sh
#
# In the source-tree case, xreal_ultra.env intentionally points XR_BIN_ROOT at
# <repo>/bin, but install_xreal_ultra_out.sh writes deployable driver variants to
# <repo>/out/xreal_ultra/bin. Prefer the packaged variant when it exists and make
# its register script resolve <package> as the runtime root. Fall back to the
# source register script with DRIVERS_ROOT pointed at the packaged driver dir.

candidate_scripts=()

add_candidate() {
  local path="$1"
  [[ -n "$path" ]] || return 0
  candidate_scripts+=("$path")
}

add_candidate "${OPENVR_REGISTER_DRIVER_SCRIPT:-}"

if [[ -n "${XR_OUT_ROOT:-}" && -n "${XR_OPENVR_DRIVER_DIR_NAME:-}" ]]; then
  add_candidate "$XR_OUT_ROOT/bin/drivers/$XR_OPENVR_DRIVER_DIR_NAME/scripts/register_driver.sh"
fi

if [[ -n "${XR_OUT_ROOT:-}" ]]; then
  add_candidate "$XR_OUT_ROOT/bin/drivers/openvr_driver/scripts/register_driver.sh"
fi

if [[ -n "${XR_ROOT_PROJECT:-}" ]]; then
  add_candidate "$XR_ROOT_PROJECT/drivers/openvr_driver/scripts/register_driver.sh"
fi

if [[ -n "${XR_BIN_ROOT:-}" ]]; then
  add_candidate "$XR_BIN_ROOT/scripts/drivers/openvr_driver/register_driver.sh"
fi

seen=()
for script in "${candidate_scripts[@]}"; do
  [[ -n "$script" ]] || continue

  # Deduplicate while preserving order.
  duplicate=0
  for prev in "${seen[@]}"; do
    if [[ "$prev" == "$script" ]]; then
      duplicate=1
      break
    fi
  done
  [[ "$duplicate" == "0" ]] || continue
  seen+=("$script")

  if [[ ! -x "$script" ]]; then
    continue
  fi

  log "using register script: $script"

  case "$script" in
    "${XR_OUT_ROOT:-__no_out_root__}"/bin/drivers/*/scripts/register_driver.sh)
      exec env \
        XR_PACKAGE_ROOT="$XR_OUT_ROOT" \
        XR_ROOT_PROJECT="$XR_OUT_ROOT" \
        ROOT_PROJECT="$XR_OUT_ROOT" \
        XR_BIN_ROOT="$XR_OUT_ROOT/bin" \
        "$script" "$@"
      ;;
    "${XR_ROOT_PROJECT:-__no_root_project__}"/drivers/openvr_driver/scripts/register_driver.sh)
      if [[ -n "${XR_OUT_ROOT:-}" && -d "$XR_OUT_ROOT/bin/drivers" ]]; then
        exec env \
          DRIVERS_ROOT="$XR_OUT_ROOT/bin/drivers" \
          "$script" "$@"
      fi
      exec "$script" "$@"
      ;;
    *)
      exec "$script" "$@"
      ;;
  esac
done

fatal "OpenVR register script not found. Tried: ${seen[*]}"
