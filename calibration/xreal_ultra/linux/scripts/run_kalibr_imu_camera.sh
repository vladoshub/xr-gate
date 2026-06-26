#!/usr/bin/env bash
set -euo pipefail

expand_tilde() {
  local value="$1"

  case "$value" in
    "~")
      printf '%s\n' "$HOME"
      ;;
    "~/"*)
      printf '%s\n' "$HOME/${value#"~/"}"
      ;;
    *)
      printf '%s\n' "$value"
      ;;
  esac
}

ROOT_PROJECT="${ROOT_PROJECT:-$HOME/src/xr_tracking}"
XREAL_CALIB_DIR="${XREAL_CALIB_DIR:-$HOME/xreal_calib}"
XREAL_RECORDS_DIR="${XREAL_RECORDS_DIR:-$HOME/xreal_records}"

BAGS_DIR="${BAGS_DIR:-$XREAL_CALIB_DIR/bags}"
TARGETS_DIR="${TARGETS_DIR:-$XREAL_CALIB_DIR/targets}"
KALIBR_RUNS_DIR="${KALIBR_RUNS_DIR:-$XREAL_CALIB_DIR/kalibr_runs}"
UNIFIED_CALIB_DIR="${UNIFIED_CALIB_DIR:-$XREAL_CALIB_DIR/unified_480_ccw90}"

BAG_PATTERN="${BAG_PATTERN:-xreal_unified480_calib_*.bag}"
CAMCHAIN_NAME="${CAMCHAIN_NAME:-camchain_unified480_ccw90.yaml}"
IMU_NAME="${IMU_NAME:-imu_xreal_air2ultra.yaml}"
TARGET_NAME="${TARGET_NAME:-aprilgrid_a4_5x7_25mm.yaml}"

FROM="${FROM:-3}"
TO="${TO:-60}"
MAX_ITER="${MAX_ITER:-6}"
TIMEOFFSET_PADDING="${TIMEOFFSET_PADDING:-0.5}"

XREAL_SERIAL="${XREAL_SERIAL:-ZBBM5DZFMP}"
XREAL_DEVICE_NAME="${XREAL_DEVICE_NAME:-xreal_air2ultra}"
CALIB_PROFILE_NAME="${CALIB_PROFILE_NAME:-unified_480_ccw90}"

DOCKER_IMAGE="${DOCKER_IMAGE:-kalibr-xreal:latest}"
DOCKER_MOUNT="${DOCKER_MOUNT:-$HOME:$HOME}"
DOCKER_INTERACTIVE="${DOCKER_INTERACTIVE:-1}"

ROOT_PROJECT="$(expand_tilde "$ROOT_PROJECT")"
XREAL_CALIB_DIR="$(expand_tilde "$XREAL_CALIB_DIR")"
XREAL_RECORDS_DIR="$(expand_tilde "$XREAL_RECORDS_DIR")"
BAGS_DIR="$(expand_tilde "$BAGS_DIR")"
TARGETS_DIR="$(expand_tilde "$TARGETS_DIR")"
KALIBR_RUNS_DIR="$(expand_tilde "$KALIBR_RUNS_DIR")"
UNIFIED_CALIB_DIR="$(expand_tilde "$UNIFIED_CALIB_DIR")"
DOCKER_MOUNT="$(expand_tilde "$DOCKER_MOUNT")"

if [[ -z "${BAG:-}" ]]; then
  BAG="$(ls -1t "$BAGS_DIR"/$BAG_PATTERN 2>/dev/null | head -n1 || true)"
fi

if [[ -z "${BAG:-}" ]]; then
  echo "[ERROR] No bag found in: $BAGS_DIR" >&2
  echo "[ERROR] Pattern: $BAG_PATTERN" >&2
  exit 1
fi

BAG="$(expand_tilde "$BAG")"

CAM="${CAM:-$UNIFIED_CALIB_DIR/$CAMCHAIN_NAME}"
CAM="$(expand_tilde "$CAM")"

IMU="${IMU:-$XREAL_CALIB_DIR/$IMU_NAME}"
IMU="$(expand_tilde "$IMU")"

TARGET="${TARGET:-$TARGETS_DIR/$TARGET_NAME}"
TARGET="$(expand_tilde "$TARGET")"

OUT="${OUT:-$KALIBR_RUNS_DIR/imu_cam_$(basename "$BAG" .bag)_${FROM}s_${TO}s}"
OUT="$(expand_tilde "$OUT")"

FINAL_ROOT="${FINAL_ROOT:-$XREAL_CALIB_DIR/final/$XREAL_DEVICE_NAME/$XREAL_SERIAL}"
FINAL_ROOT="$(expand_tilde "$FINAL_ROOT")"

FINAL="${FINAL:-$FINAL_ROOT/$CALIB_PROFILE_NAME}"
FINAL="$(expand_tilde "$FINAL")"

for required_file in "$BAG" "$CAM" "$IMU" "$TARGET"; do
  if [[ ! -f "$required_file" ]]; then
    echo "[ERROR] Required file not found: $required_file" >&2
    exit 1
  fi
done

mkdir -p "$OUT"
mkdir -p "$FINAL"

echo "ROOT_PROJECT=$ROOT_PROJECT"
echo "XREAL_CALIB_DIR=$XREAL_CALIB_DIR"
echo "XREAL_RECORDS_DIR=$XREAL_RECORDS_DIR"
echo "BAGS_DIR=$BAGS_DIR"
echo "TARGETS_DIR=$TARGETS_DIR"
echo "KALIBR_RUNS_DIR=$KALIBR_RUNS_DIR"
echo "UNIFIED_CALIB_DIR=$UNIFIED_CALIB_DIR"
echo "BAG=$BAG"
echo "CAM=$CAM"
echo "IMU=$IMU"
echo "TARGET=$TARGET"
echo "FROM=$FROM"
echo "TO=$TO"
echo "MAX_ITER=$MAX_ITER"
echo "TIMEOFFSET_PADDING=$TIMEOFFSET_PADDING"
echo "OUT=$OUT"
echo "XREAL_SERIAL=$XREAL_SERIAL"
echo "XREAL_DEVICE_NAME=$XREAL_DEVICE_NAME"
echo "CALIB_PROFILE_NAME=$CALIB_PROFILE_NAME"
echo "FINAL=$FINAL"
echo "DOCKER_IMAGE=$DOCKER_IMAGE"
echo "DOCKER_MOUNT=$DOCKER_MOUNT"

docker_args=(--rm)

if [[ "$DOCKER_INTERACTIVE" == "1" ]]; then
  docker_args+=(-it)
fi

docker_args+=(
  -v "$DOCKER_MOUNT"
  -w "$OUT"
  -e BAG="$BAG"
  -e CAM="$CAM"
  -e IMU="$IMU"
  -e TARGET="$TARGET"
  -e FROM="$FROM"
  -e TO="$TO"
  -e MAX_ITER="$MAX_ITER"
  -e TIMEOFFSET_PADDING="$TIMEOFFSET_PADDING"
  "$DOCKER_IMAGE"
  bash -lc '
    set -e
    export MPLBACKEND=Agg
    source /kalibr/catkin_ws/devel/setup.bash

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
  echo
  echo "[WARN] Kalibr exited with code $KALIBR_EXIT."
  echo "[WARN] If camchain/results files exist, this may be only report plotting failure."
fi

echo
echo "[files in OUT]"
find "$OUT" -maxdepth 1 -type f -print

CAMCHAIN_IMU="$(find "$OUT" -maxdepth 1 -name 'camchain-imucam-*.yaml' | head -n1)"
IMU_OUT="$(find "$OUT" -maxdepth 1 -name 'imu-*.yaml' | head -n1)"
RESULTS="$(find "$OUT" -maxdepth 1 -name 'results-imucam-*.txt' | head -n1)"
REPORT="$(find "$OUT" -maxdepth 1 -name 'report-imucam-*.pdf' | head -n1)"

if [[ -z "$CAMCHAIN_IMU" ]]; then
  echo "[ERROR] camchain-imucam-*.yaml not found in $OUT" >&2
  exit 1
fi

cp "$CAMCHAIN_IMU" "$FINAL/camchain-imucam.yaml"

if [[ -n "$IMU_OUT" ]]; then
  cp "$IMU_OUT" "$FINAL/imu.yaml"
fi

if [[ -n "$RESULTS" ]]; then
  cp "$RESULTS" "$FINAL/results-imucam.txt"
fi

if [[ -n "$REPORT" ]]; then
  cp "$REPORT" "$FINAL/report-imucam.pdf"
fi

cp "$TARGET" "$FINAL/aprilgrid.yaml"
cp "$CAM" "$FINAL/camchain-camera-only.yaml"

echo
echo "[OK] installed final profile:"
ls -lh "$FINAL"

echo
echo "[results preview]"
if [[ -f "$FINAL/results-imucam.txt" ]]; then
  sed -n '1,220p' "$FINAL/results-imucam.txt"
fi
