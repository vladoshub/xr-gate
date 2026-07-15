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

unset CAPTURE_PROFILE_PROBE_ENABLED CAPTURE_PROFILE_PROBE_BIN
for profile in "${profiles[@]}"; do
  printf '{"profile":"%s"}\n' "$profile" > "$REGISTRY"
  resolved="$(capture_profile_resolve_name '' "$REGISTRY" xreal_air2ultra_unified_480 "$ROOT_PROJECT")"
  [[ "$resolved" == "$profile" ]]

  for backend in "${backends[@]}"; do
    file="$(capture_profile_find_file \
      "$backend" "$resolved" "$ROOT_PROJECT" \
      "$ROOT_PROJECT/backends/$backend/scripts/linux")"
    [[ -f "$file" ]]
    [[ "$(basename "$file")" == "$profile.env" ]]
  done
done

cat > "$TMP_DIR/fake_capture_tcp_probe" <<'PROBE'
#!/usr/bin/env bash
set -euo pipefail
[[ " $* " == *" --print-profile "* ]]
printf '%s\n' "${FAKE_CAPTURE_PROFILE:-leap_motion_uvc_nrf54l15}"
PROBE
chmod +x "$TMP_DIR/fake_capture_tcp_probe"

printf '{"profile":"leap_motion_uvc"}\n' > "$REGISTRY"
export CAPTURE_PROFILE_PROBE_ENABLED=1
export CAPTURE_PROFILE_PROBE_BIN="$TMP_DIR/fake_capture_tcp_probe"
export FAKE_CAPTURE_PROFILE=leap_motion_uvc_nrf54l15

# Launcher CLI option is stripped, validated and kept above env/probe/registry.
capture_profile_parse_cli --backend-flag 7 --capture-profile=xreal_air2ultra_unified_480 --tail
[[ "$CAPTURE_PROFILE_CLI_OVERRIDE" == "xreal_air2ultra_unified_480" ]]
[[ "${CAPTURE_PROFILE_FORWARD_ARGS[*]}" == "--backend-flag 7 --tail" ]]

# Explicit override wins over probe and registry.
capture_profile_resolve "$CAPTURE_PROFILE_CLI_OVERRIDE" "$REGISTRY" fallback "$ROOT_PROJECT"
[[ "$CAPTURE_PROFILE_RESOLVED" == "xreal_air2ultra_unified_480" ]]
[[ "$CAPTURE_PROFILE_SOURCE_RESOLVED" == "explicit" ]]

# Probe wins over local registry when explicitly enabled.
capture_profile_resolve '' "$REGISTRY" fallback "$ROOT_PROJECT"
[[ "$CAPTURE_PROFILE_RESOLVED" == "leap_motion_uvc_nrf54l15" ]]
[[ "$CAPTURE_PROFILE_SOURCE_RESOLVED" == "tcp_probe" ]]

# With probing disabled, local registry wins.
export CAPTURE_PROFILE_PROBE_ENABLED=0
capture_profile_resolve '' "$REGISTRY" fallback "$ROOT_PROJECT"
[[ "$CAPTURE_PROFILE_RESOLVED" == "leap_motion_uvc" ]]
[[ "$CAPTURE_PROFILE_SOURCE_RESOLVED" == "registry" ]]

# Missing registry falls back to the legacy XREAL profile.
rm -f "$REGISTRY"
capture_profile_resolve '' "$REGISTRY" xreal_air2ultra_unified_480 "$ROOT_PROJECT"
[[ "$CAPTURE_PROFILE_RESOLVED" == "xreal_air2ultra_unified_480" ]]
[[ "$CAPTURE_PROFILE_SOURCE_RESOLVED" == "fallback" ]]

if capture_profile_validate_name '../invalid' >/dev/null 2>&1; then
  echo "invalid profile name was accepted" >&2
  exit 1
fi

echo "capture profile smoke test passed"
