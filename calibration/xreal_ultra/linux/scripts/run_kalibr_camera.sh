#!/usr/bin/env bash
set -euo pipefail

BAG="${BAG:-$(ls -1t "$HOME"/xreal_calib/bags/xreal_unified480_calib_*.bag | head -n1)}"
TARGET="${TARGET:-$HOME/xreal_calib/targets/aprilgrid_a4_5x7_25mm.yaml}"
OUT="${OUT:-$HOME/xreal_calib/kalibr_runs/camera_$(basename "$BAG" .bag)}"

mkdir -p "$OUT"

echo "BAG=$BAG"
echo "TARGET=$TARGET"
echo "OUT=$OUT"

docker run --rm -it \
  -v "$HOME:$HOME" \
  -w "$OUT" \
  kalibr-xreal:latest \
  bash -lc "
    set -e
    export MPLBACKEND=Agg
    source /kalibr/catkin_ws/devel/setup.bash

    kalibr_calibrate_cameras \
      --bag '$BAG' \
      --topics /cam0/image_raw /cam1/image_raw \
      --models pinhole-equi pinhole-equi \
      --target '$TARGET' \
      --dont-show-report
  "

CAMCHAIN="$(find "$OUT" -maxdepth 1 -name 'camchain-*.yaml' | head -n1)"

if [ -z "$CAMCHAIN" ]; then
  echo "ERROR: camchain was not produced in $OUT"
  find "$OUT" -maxdepth 1 -type f -print
  exit 1
fi

mkdir -p "$HOME/xreal_calib/unified_480_ccw90"

cp "$CAMCHAIN" "$HOME/xreal_calib/unified_480_ccw90/camchain_unified480_ccw90.yaml"

echo
echo "[OK] camera calibration done"
echo "CAMCHAIN=$CAMCHAIN"
echo "COPIED_TO=$HOME/xreal_calib/unified_480_ccw90/camchain_unified480_ccw90.yaml"

echo
echo "[check resolution]"
grep -nA8 'resolution' "$HOME/xreal_calib/unified_480_ccw90/camchain_unified480_ccw90.yaml" || true

echo
echo "[files]"
ls -lh "$OUT"
