#!/usr/bin/env bash
# Compatibility entrypoint for the generic XR Gate workflow.
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
XR_ROOT_PROJECT="${XR_ROOT_PROJECT:-$(cd "$SCRIPT_DIR/../../../.." && pwd)}"
export XR_ROOT_PROJECT
exec "$XR_ROOT_PROJECT/devices/common/linux/scripts/ci/run_xr_gate_act_build.sh" "$@"
