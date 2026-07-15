#!/usr/bin/env bash
set -euo pipefail

selected() {
  local token
  [[ -z "${XR_BUILD_ONLY:-}" ]] && return 0
  for token in ${XR_BUILD_ONLY}; do
    case "$token" in
      all|everything|device|device_components|xreal_display_helper) return 0 ;;
    esac
  done
  return 1
}

if ! selected; then
  echo "[build_xreal_ultra_components] skip xreal_display_helper due XR_BUILD_ONLY='${XR_BUILD_ONLY:-}'" >&2
  exit 0
fi

: "${XR_ROOT_PROJECT:?XR_ROOT_PROJECT is required}"
: "${XR_OUT_BIN_ROOT:?XR_OUT_BIN_ROOT is required}"

echo "[build_xreal_ultra_components] == xreal_display_helper ==" >&2
INSTALL_BIN_DIR="$XR_OUT_BIN_ROOT/xreal_display_helper" \
  bash "$XR_ROOT_PROJECT/tools/xreal_ultra/xreal_display_helper/scripts/linux/install_xreal_helper.sh"
