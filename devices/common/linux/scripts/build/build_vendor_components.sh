#!/usr/bin/env bash
set -euo pipefail

is_enabled() {
  case "${1:-}" in
    1|true|TRUE|yes|YES|on|ON) return 0 ;;
    *) return 1 ;;
  esac
}

if ! is_enabled "${XR_BUILD_VENDOR_COMPONENTS:-1}"; then
  echo "[build_vendor_components] disabled by XR_BUILD_VENDOR_COMPONENTS=${XR_BUILD_VENDOR_COMPONENTS:-0}" >&2
  exit 0
fi

: "${XR_ROOT_PROJECT:?XR_ROOT_PROJECT is required}"

for vendor in ${XR_VENDOR_COMPONENTS:-xreal_ultra}; do
  case "$vendor" in
    xreal_ultra)
      hook="$XR_ROOT_PROJECT/devices/xreal_ultra/linux/scripts/build_xreal_ultra_components.sh"
      ;;
    *)
      echo "[build_vendor_components][ERROR] unknown vendor component set: $vendor" >&2
      exit 2
      ;;
  esac
  [[ -x "$hook" ]] || {
    echo "[build_vendor_components][ERROR] hook is not executable: $hook" >&2
    exit 2
  }
  echo "[build_vendor_components] == $vendor ==" >&2
  "$hook"
done
