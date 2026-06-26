#!/usr/bin/env bash
set -euo pipefail

ROOT_PROJECT="${ROOT_PROJECT:-$HOME/src/xr_tracking}"
REMOVE="${REMOVE:-0}"

cd "$ROOT_PROJECT"

candidates=(
  "src/build"
  "src/capture_net_bridge.cpp"
  "src/tracking_udp_bridge.cpp"
  "src/tracking_udp_debug_receiver.cpp"
  "src/xr_video_backend.cpp"
  "src/xr_video_monitor.cpp"
  "src/capture_basalt_backend.cpp"
  "src/capture_hand_tracking_backend.cpp"
)

# These are not removed by default because they should be moved/confirmed first.
review=(
  "src/xr_runtime_adapter.cpp"
  "src/capture_stereo_rate_check.cpp"
  "src/CMakeLists.txt"
)

echo "[cleanup_old_flat_sources] removable candidates:"
for p in "${candidates[@]}"; do
  [[ -e "$p" ]] && echo "  $p"
done

echo
echo "[cleanup_old_flat_sources] review before deleting:"
for p in "${review[@]}"; do
  [[ -e "$p" ]] && echo "  $p"
done

if [[ "$REMOVE" != "1" ]]; then
  echo
echo "Dry run only. Run with REMOVE=1 to delete removable candidates."
  exit 0
fi

for p in "${candidates[@]}"; do
  [[ -e "$p" ]] && rm -rf "$p"
done

echo "[cleanup_old_flat_sources] removed candidates"
