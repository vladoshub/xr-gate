#!/usr/bin/env bash
set -euo pipefail

ROOT_PROJECT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
# shellcheck source=/dev/null
source "$ROOT_PROJECT/backends/common/scripts/linux/capture_profile.sh"

TMP_DIR="$(mktemp -d)"
trap 'rm -rf "$TMP_DIR"' EXIT
REGISTRY="$TMP_DIR/capture_service_streams.json"

profiles=(
  xreal_air2ultra_unified_480
  leap_motion_uvc
  leap_motion_uvc_nrf54l15
  leap_motion_uvc_xreal_imu
)
backends=(
  mercury_hand_tracking
  basalt_vio
  imu_3dof
  xr_video
  xr_spatial
)

for profile in "${profiles[@]}"; do
  printf '{"profile":"%s"}\n' "$profile" > "$REGISTRY"
  resolved="$(capture_profile_resolve_name '' "$REGISTRY" xreal_air2ultra_unified_480)"
  [[ "$resolved" == "$profile" ]]

  for backend in "${backends[@]}"; do
    file="$(capture_profile_find_file \
      "$backend" "$resolved" "$ROOT_PROJECT" \
      "$ROOT_PROJECT/backends/$backend/scripts/linux")"
    [[ -f "$file" ]]
    [[ "$(basename "$file")" == "$profile.env" ]]
  done
done

printf '{"namespace":"legacy-xreal"}\n' > "$REGISTRY"
[[ "$(capture_profile_resolve_name '' "$REGISTRY" xreal_air2ultra_unified_480)" == \
   "xreal_air2ultra_unified_480" ]]

if capture_profile_validate_name '../invalid' >/dev/null 2>&1; then
  echo "invalid profile name was accepted" >&2
  exit 1
fi

echo "capture profile smoke test passed"
