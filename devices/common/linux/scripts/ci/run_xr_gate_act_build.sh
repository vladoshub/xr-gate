#!/usr/bin/env bash
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
XR_ROOT_PROJECT="${XR_ROOT_PROJECT:-$(cd "$SCRIPT_DIR/../../../../.." && pwd)}"
export XR_ROOT_PROJECT
export XR_ACT_DEFAULT_WORKFLOW="${XR_ACT_DEFAULT_WORKFLOW:-$XR_ROOT_PROJECT/.github/workflows/xr-gate-split-build.yml}"
export XR_ACT_LOG_STEM="${XR_ACT_LOG_STEM:-xr_gate}"
export XR_ACT_MAIN_ARTIFACT_HINT="${XR_ACT_MAIN_ARTIFACT_HINT:-xr-gate-linux-x64.zip / xr-gate-linux-x64.tar.gz}"
export XR_ACT_MODELS_ARTIFACT_HINT="${XR_ACT_MODELS_ARTIFACT_HINT:-hand-tracking-models-mercury.zip / hand-tracking-models-mercury.tar.gz}"

forward=()
while [[ $# -gt 0 ]]; do
  case "$1" in
    --no-vendor-components)
      forward+=(--input include_vendor_components=false)
      ;;
    --vendor-components)
      forward+=(--input include_vendor_components=true)
      ;;
    --with-xrizer)
      forward+=(--input include_xrizer=true)
      ;;
    --without-xrizer)
      forward+=(--input include_xrizer=false)
      ;;
    -h|--help)
      cat <<'EOF'
XR Gate act wrapper additions:
  --no-vendor-components  Build without pure vendor components.
  --vendor-components     Explicitly include vendor components (default).
  --with-xrizer           Build xrizer and package its GPL license and Corresponding Source.
  --without-xrizer        Explicitly disable xrizer (default).

All other options are forwarded to devices/common/linux/scripts/ci/run_act_build.sh.
EOF
      exec "$SCRIPT_DIR/run_act_build.sh" --help
      ;;
    *) forward+=("$1") ;;
  esac
  shift
done

exec "$SCRIPT_DIR/run_act_build.sh" "${forward[@]}"
