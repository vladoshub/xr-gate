#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=load_target.sh
source "$SCRIPT_DIR/load_target.sh"

BAG="${BAG:-$(ls -1t "$BAGS_DIR"/$BAG_PATTERN 2>/dev/null | head -n1 || true)}"
[[ -n "$BAG" ]] || { echo "[camera-calib][ERROR] no bag matching $BAGS_DIR/$BAG_PATTERN" >&2; exit 1; }
BAG="$(expand_tilde "$BAG")"
TARGET="$(expand_tilde "${TARGET:-$APRILGRID_PATH}")"
OUT="$(expand_tilde "${OUT:-$KALIBR_RUNS_DIR/camera_$(basename "$BAG" .bag)}")"
mkdir -p "$OUT"

for file in "$BAG" "$TARGET"; do
  [[ -f "$file" ]] || { echo "[camera-calib][ERROR] required file missing: $file" >&2; exit 1; }
done

print_target_summary
echo "BAG=$BAG"
echo "TARGET=$TARGET"
echo "OUT=$OUT"
echo "MODELS=$CAMERA_MODEL_0 $CAMERA_MODEL_1"

docker run --rm -it \
  -v "$HOME:$HOME" \
  -w "$OUT" \
  "$DOCKER_IMAGE" \
  bash -lc "
    set -e
    export MPLBACKEND=Agg
    source '$ROS_SETUP'
    kalibr_calibrate_cameras \\
      --bag '$BAG' \\
      --topics '$CAM0_TOPIC' '$CAM1_TOPIC' \\
      --models '$CAMERA_MODEL_0' '$CAMERA_MODEL_1' \\
      --target '$TARGET' \\
      --dont-show-report
  "

CAMCHAIN="$(find "$OUT" -maxdepth 1 -name 'camchain-*.yaml' | head -n1)"
[[ -n "$CAMCHAIN" ]] || { echo "[camera-calib][ERROR] camchain not produced in $OUT" >&2; exit 1; }

echo
echo "[OK] camera calibration complete"
echo "CAMCHAIN=$CAMCHAIN"
echo "Run save_camera_calibration.sh to install the profile."
