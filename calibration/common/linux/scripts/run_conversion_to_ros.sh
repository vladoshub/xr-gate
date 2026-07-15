#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=load_target.sh
source "$SCRIPT_DIR/load_target.sh"

DS="${DS:-$(ls -1dt "$RECORDS_DIR"/$DATASET_GLOB 2>/dev/null | head -n1 || true)}"
if [[ -z "$DS" ]]; then
  echo "[conversion][ERROR] no dataset found in $RECORDS_DIR matching $DATASET_GLOB" >&2
  exit 1
fi
DS="$(expand_tilde "$DS")"
BAG="$(expand_tilde "${BAG:-$BAGS_DIR/$(basename "$DS").bag}")"
mkdir -p "$BAGS_DIR"

NO_IMU_ARG=()
if [[ "${NO_IMU:-auto}" == "1" ]]; then
  NO_IMU_ARG=(--no-imu)
elif [[ "${NO_IMU:-auto}" == "auto" ]]; then
  if [[ ! -f "$DS/imu.csv" ]] || [[ "$(wc -l < "$DS/imu.csv")" -le 1 ]]; then
    NO_IMU_ARG=(--no-imu)
  fi
fi

print_target_summary
echo "DS=$DS"
echo "BAG=$BAG"
echo "CONVERTER_TO_BAG=$CONVERTER_TO_BAG"
echo "NO_IMU=${NO_IMU_ARG[*]:-0}"

DOCKER_MOUNT_HOST="${DOCKER_MOUNT_HOST:-$HOME}"
DOCKER_MOUNT_CONTAINER="${DOCKER_MOUNT_CONTAINER:-$HOME}"

docker run --rm -it \
  -v "$DOCKER_MOUNT_HOST:$DOCKER_MOUNT_CONTAINER" \
  -w "$XR_CALIB_COMMON_DIR" \
  -e DS="$DS" \
  -e BAG="$BAG" \
  -e CONVERTER="$CONVERTER_TO_BAG" \
  -e ROS_SETUP="$ROS_SETUP" \
  -e NO_IMU_ARGS="${NO_IMU_ARG[*]:-}" \
  "$DOCKER_IMAGE" \
  bash -lc '
    set -euo pipefail
    source "$ROS_SETUP"
    args=(python "$CONVERTER" --dataset "$DS" --out "$BAG")
    if [[ -n "$NO_IMU_ARGS" ]]; then args+=(--no-imu); fi
    "${args[@]}"
    rosbag info "$BAG"
  '
