#!/usr/bin/env bash
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
XR_ROOT_PROJECT="${XR_ROOT_PROJECT:-$(cd "$SCRIPT_DIR/../../../.." && pwd)}"
export XR_ROOT_PROJECT
export XR_ACT_DEFAULT_WORKFLOW="${XR_ACT_DEFAULT_WORKFLOW:-$XR_ROOT_PROJECT/.github/workflows/xreal-ultra-split-build.yml}"
export XR_ACT_LOG_STEM="${XR_ACT_LOG_STEM:-xreal_ultra}"
export XR_ACT_MAIN_ARTIFACT_HINT="${XR_ACT_MAIN_ARTIFACT_HINT:-xreal-ultra-linux-x64.zip / xreal_ultra_linux_x64.tar.gz}"
export XR_ACT_MODELS_ARTIFACT_HINT="${XR_ACT_MODELS_ARTIFACT_HINT:-hand-tracking-models-mercury.zip / hand-tracking-models-mercury.tar.gz}"
exec "$XR_ROOT_PROJECT/devices/common/linux/scripts/ci/run_act_build.sh" "$@"
