#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT_ENV="$SCRIPT_DIR/../xreal_ultra_out_env.sh"
if [[ -f "$OUT_ENV" ]]; then
  # Source-tree mode: default to out/xreal_ultra/bin/hand-tracking-models/mercury.
  # shellcheck source=/dev/null
  source "$OUT_ENV"
else
  # Runtime package mode: default to this package's bin/hand-tracking-models/mercury.
  DEVICE_HOME="$(cd "$SCRIPT_DIR/../../.." && pwd)"
  # shellcheck source=/dev/null
  source "$DEVICE_HOME/xreal_ultra.env"
fi

export ROOT_PROJECT="${ROOT_PROJECT:-$XR_ROOT_PROJECT}"
export MERCURY_MODELS="${MERCURY_MODELS:-${XR_OUT_BIN_ROOT:-$XR_BIN_ROOT}/hand-tracking-models/mercury}"

SCRIPT="$XR_BIN_ROOT/scripts/backends/mercury_hand_tracking/download_mercury_models.sh"
if [[ ! -x "$SCRIPT" ]]; then
  SCRIPT="$XR_ROOT_PROJECT/backends/mercury_hand_tracking/scripts/linux/download_mercury_models.sh"
fi

exec "$SCRIPT" "$@"
