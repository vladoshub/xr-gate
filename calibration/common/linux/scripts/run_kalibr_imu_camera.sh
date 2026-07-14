#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=load_target.sh
source "$SCRIPT_DIR/load_target.sh"

FROM="${FROM:-3}"
TO="${TO:-60}"
MAX_ITER="${MAX_ITER:-6}"
TIMEOFFSET_PADDING="${TIMEOFFSET_PADDING:-0.5}"
DOCKER_INTERACTIVE="${DOCKER_INTERACTIVE:-1}"

BAG="${BAG:-$(ls -1t "$BAGS_DIR"/$BAG_PATTERN 2>/dev/null | head -n1 || true)}"
[[ -n "$BAG" ]] || { echo "[imu-camera][ERROR] no bag matching $BAGS_DIR/$BAG_PATTERN" >&2; exit 1; }
BAG="$(expand_tilde "$BAG")"
CAM="$(expand_tilde "${CAM:-$CAMERA_PROFILE_DIR/$CAMCHAIN_OUTPUT_NAME}")"
IMU="$(expand_tilde "${IMU:-$IMU_YAML_PATH}")"
TARGET="$(expand_tilde "${TARGET:-$APRILGRID_PATH}")"
OUT="$(expand_tilde "${OUT:-$KALIBR_RUNS_DIR/imu_cam_$(basename "$BAG" .bag)_${FROM}s_${TO}s}")"
FINAL="$(expand_tilde "${FINAL:-$FINAL_PROFILE_DIR}")"

for file in "$BAG" "$CAM" "$IMU" "$TARGET"; do
  [[ -f "$file" ]] || { echo "[imu-camera][ERROR] required file missing: $file" >&2; exit 1; }
done
mkdir -p "$OUT" "$FINAL"

print_target_summary
echo "BAG=$BAG"
echo "CAM=$CAM"
echo "IMU=$IMU"
echo "TARGET=$TARGET"
echo "FROM=$FROM TO=$TO MAX_ITER=$MAX_ITER TIMEOFFSET_PADDING=$TIMEOFFSET_PADDING"
echo "OUT=$OUT"
echo "FINAL=$FINAL"

docker_args=(--rm)
[[ "$DOCKER_INTERACTIVE" == "1" ]] && docker_args+=(-it)
docker_args+=(
  -v "$HOME:$HOME"
  -w "$OUT"
  -e BAG="$BAG"
  -e CAM="$CAM"
  -e IMU="$IMU"
  -e TARGET="$TARGET"
  -e FROM="$FROM"
  -e TO="$TO"
  -e MAX_ITER="$MAX_ITER"
  -e TIMEOFFSET_PADDING="$TIMEOFFSET_PADDING"
  -e ROS_SETUP="$ROS_SETUP"
  "$DOCKER_IMAGE"
  bash -lc '
    set -e
    export MPLBACKEND=Agg
    source "$ROS_SETUP"
    kalibr_calibrate_imu_camera \
      --bag "$BAG" \
      --cam "$CAM" \
      --imu "$IMU" \
      --target "$TARGET" \
      --bag-from-to "$FROM" "$TO" \
      --timeoffset-padding "$TIMEOFFSET_PADDING" \
      --dont-show-report \
      --max-iter "$MAX_ITER"
  '
)

set +e
docker run "${docker_args[@]}"
KALIBR_EXIT=$?
set -e
if [[ "$KALIBR_EXIT" -ne 0 ]]; then
  echo "[imu-camera][WARN] Kalibr exited with code $KALIBR_EXIT; continuing if output files exist." >&2
fi

CAMCHAIN_IMU="$(find "$OUT" -maxdepth 1 -name 'camchain-imucam-*.yaml' | head -n1)"
IMU_OUT="$(find "$OUT" -maxdepth 1 -name 'imu-*.yaml' | head -n1)"
RESULTS="$(find "$OUT" -maxdepth 1 -name 'results-imucam-*.txt' | head -n1)"
REPORT="$(find "$OUT" -maxdepth 1 -name 'report-imucam-*.pdf' | head -n1)"
[[ -n "$CAMCHAIN_IMU" ]] || { echo "[imu-camera][ERROR] camchain-imucam output not found in $OUT" >&2; exit 1; }

cp "$CAMCHAIN_IMU" "$FINAL/camchain-imucam.yaml"
[[ -n "$IMU_OUT" ]] && cp "$IMU_OUT" "$FINAL/imu.yaml"
[[ -n "$RESULTS" ]] && cp "$RESULTS" "$FINAL/results-imucam.txt"
[[ -n "$REPORT" ]] && cp "$REPORT" "$FINAL/report-imucam.pdf"
cp "$TARGET" "$FINAL/aprilgrid.yaml"
cp "$CAM" "$FINAL/camchain-camera-only.yaml"

echo "[OK] installed final stereo+IMU profile:"
ls -lh "$FINAL"
[[ -f "$FINAL/results-imucam.txt" ]] && sed -n '1,220p' "$FINAL/results-imucam.txt"
