#!/usr/bin/env bash
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
_models_was_set="${MERCURY_MODELS+x}"
# shellcheck source=/dev/null
source "$SCRIPT_DIR/../lib/device_runtime.sh"
xr_load_device_env

# Preserve the old source-tree convenience: when an existing device output tree
# is present and the caller did not explicitly choose MERCURY_MODELS, install
# models into that deploy tree. Runtime packages continue using XR_BIN_ROOT.
if [[ -z "$_models_was_set" && -d "$XR_ROOT_PROJECT/out/$XR_TARGET_DEVICE/bin" ]]; then
  export MERCURY_MODELS="$XR_ROOT_PROJECT/out/$XR_TARGET_DEVICE/bin/hand-tracking-models/mercury"
fi

xr_exec_runtime_script mercury_models \
  scripts/backends/mercury_hand_tracking/download_mercury_models.sh \
  backends/mercury_hand_tracking/scripts/linux/download_mercury_models.sh \
  "$@"
