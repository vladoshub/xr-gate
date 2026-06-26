#!/usr/bin/env bash
set -euo pipefail

# Project roots/directories
ROOT_PROJECT="${ROOT_PROJECT:-$HOME/src/xr_tracking}"
XREAL_CALIB_DIR="${XREAL_CALIB_DIR:-$HOME/xreal_calib}"
XREAL_RECORDS_DIR="${XREAL_RECORDS_DIR:-$HOME/xreal_records}"

# Allow values like ROOT_PROJECT="~/src/xr_tracking"
ROOT_PROJECT="${ROOT_PROJECT/#\~/$HOME}"
XREAL_CALIB_DIR="${XREAL_CALIB_DIR/#\~/$HOME}"
XREAL_RECORDS_DIR="${XREAL_RECORDS_DIR/#\~/$HOME}"

# Kalibr/Docker settings
DOCKER_IMAGE="${DOCKER_IMAGE:-kalibr-xreal:latest}"
ROS_SETUP="${ROS_SETUP:-/kalibr/catkin_ws/devel/setup.bash}"
DOCKER_MOUNT_HOST="${DOCKER_MOUNT_HOST:-$HOME}"
DOCKER_MOUNT_CONTAINER="${DOCKER_MOUNT_CONTAINER:-$HOME}"

# Conversion paths
CALIBRATION_DIR="${CALIBRATION_DIR:-$ROOT_PROJECT/calibration/xreal_ultra/linux}"
CONVERTER="${CONVERTER:-$ROOT_PROJECT/calibration/xreal_ultra/linux/convert_unified_dataset_to_kalibr_bag.py}"
BAGS_DIR="${BAGS_DIR:-$XREAL_CALIB_DIR/bags}"

# Dataset discovery
DATASET_GLOB="${DATASET_GLOB:-xreal_unified480_calib_*}"
DS="${DS:-$(ls -1dt "$XREAL_RECORDS_DIR"/$DATASET_GLOB 2>/dev/null | head -n1 || true)}"

if [[ -z "$DS" ]]; then
  echo "ERROR: no dataset found in $XREAL_RECORDS_DIR matching $DATASET_GLOB" >&2
  echo "Set DS=/path/to/dataset explicitly or check XREAL_RECORDS_DIR/DATASET_GLOB." >&2
  exit 1
fi

BAG="${BAG:-$BAGS_DIR/$(basename "$DS").bag}"

mkdir -p "$BAGS_DIR"

echo "ROOT_PROJECT=$ROOT_PROJECT"
echo "XREAL_CALIB_DIR=$XREAL_CALIB_DIR"
echo "XREAL_RECORDS_DIR=$XREAL_RECORDS_DIR"
echo "BAGS_DIR=$BAGS_DIR"
echo "DS=$DS"
echo "BAG=$BAG"
echo "CALIBRATION_DIR=$CALIBRATION_DIR"
echo "CONVERTER=$CONVERTER"
echo "DOCKER_IMAGE=$DOCKER_IMAGE"

docker run --rm -it \
  -v "$DOCKER_MOUNT_HOST:$DOCKER_MOUNT_CONTAINER" \
  -w "$CALIBRATION_DIR" \
  -e DS="$DS" \
  -e BAG="$BAG" \
  -e CONVERTER="$CONVERTER" \
  -e ROS_SETUP="$ROS_SETUP" \
  "$DOCKER_IMAGE" \
  bash -lc '
    set -euo pipefail

    source "$ROS_SETUP"

    echo "Using python:"
    python --version

    python "$CONVERTER" \
      --dataset "$DS" \
      --out "$BAG"

    rosbag info "$BAG"
  '
