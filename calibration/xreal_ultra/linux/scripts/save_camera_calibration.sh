#!/usr/bin/env bash
set -euo pipefail

# Project/calibration roots
ROOT_PROJECT="${ROOT_PROJECT:-$HOME/src/xr_tracking}"
XREAL_CALIB_DIR="${XREAL_CALIB_DIR:-$HOME/xreal_calib}"

# Derived directories
BAGS_DIR="${BAGS_DIR:-$XREAL_CALIB_DIR/bags}"
KALIBR_RUNS_DIR="${KALIBR_RUNS_DIR:-$XREAL_CALIB_DIR/kalibr_runs}"
FINAL_CAM_DIR="${FINAL_CAM_DIR:-$XREAL_CALIB_DIR/unified_480_ccw90}"

# File naming
BAG_PATTERN="${BAG_PATTERN:-xreal_unified480_calib_*.bag}"
CAMCHAIN_OUTPUT_NAME="${CAMCHAIN_OUTPUT_NAME:-camchain_unified480_ccw90.yaml}"
RESULTS_OUTPUT_NAME="${RESULTS_OUTPUT_NAME:-results-cam_unified480_ccw90.txt}"

# Allow paths like ROOT_PROJECT="~/src/xr_tracking"
ROOT_PROJECT="${ROOT_PROJECT/#\~/$HOME}"
XREAL_CALIB_DIR="${XREAL_CALIB_DIR/#\~/$HOME}"
BAGS_DIR="${BAGS_DIR/#\~/$HOME}"
KALIBR_RUNS_DIR="${KALIBR_RUNS_DIR/#\~/$HOME}"
FINAL_CAM_DIR="${FINAL_CAM_DIR/#\~/$HOME}"

# Inputs/outputs
BAG="${BAG:-$(ls -1t "$BAGS_DIR"/$BAG_PATTERN | head -n1)}"
OUT="${OUT:-$KALIBR_RUNS_DIR/camera_$(basename "$BAG" .bag)}"

OUT="${OUT/#\~/$HOME}"
BAG="${BAG/#\~/$HOME}"

CAMCHAIN_FINAL="$FINAL_CAM_DIR/$CAMCHAIN_OUTPUT_NAME"
RESULTS_FINAL="$FINAL_CAM_DIR/$RESULTS_OUTPUT_NAME"

echo "ROOT_PROJECT=$ROOT_PROJECT"
echo "XREAL_CALIB_DIR=$XREAL_CALIB_DIR"
echo "BAGS_DIR=$BAGS_DIR"
echo "KALIBR_RUNS_DIR=$KALIBR_RUNS_DIR"
echo "BAG=$BAG"
echo "OUT=$OUT"
echo "FINAL_CAM_DIR=$FINAL_CAM_DIR"
echo "CAMCHAIN_FINAL=$CAMCHAIN_FINAL"
echo "RESULTS_FINAL=$RESULTS_FINAL"

if [ ! -d "$OUT" ]; then
  echo "ERROR: OUT directory does not exist: $OUT"
  echo
  echo "Existing camera runs:"
  find "$KALIBR_RUNS_DIR" -maxdepth 1 -type d -name 'camera_*' -print 2>/dev/null || true
  exit 1
fi

CAMCHAIN="$(find "$OUT" -maxdepth 1 -name 'camchain-*.yaml' | head -n1)"
RESULTS="$(find "$OUT" -maxdepth 1 -name 'results-cam-*.txt' | head -n1)"

if [ -z "$CAMCHAIN" ]; then
  echo "ERROR: camchain-*.yaml not found in $OUT"
  find "$OUT" -maxdepth 1 -type f -print
  exit 1
fi

if [ -z "$RESULTS" ]; then
  echo "WARN: results-cam-*.txt not found in $OUT"
fi

mkdir -p "$FINAL_CAM_DIR"

cp "$CAMCHAIN" "$CAMCHAIN_FINAL"

if [ -n "$RESULTS" ]; then
  cp "$RESULTS" "$RESULTS_FINAL"
fi

echo
echo "[OK] copied:"
ls -lh "$FINAL_CAM_DIR"

echo
echo "[check camchain]"
grep -nE 'cam0:|cam1:|camera_model|distortion_model|intrinsics|distortion_coeffs|resolution|T_cn_cnm1' \
  "$CAMCHAIN_FINAL" || true
