#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TARGET="${CALIB_TARGET:-xreal_ultra}"

usage() {
  cat <<USAGE
Usage: $0 [--target NAME] COMMAND

Targets:
  xreal_ultra
  leap_motion_uvc_nrf54l15

Commands:
  show-target       Print resolved target configuration
  install           Install/build Kalibr environment and create target YAML files
  record            Record camera-only or stereo+IMU dataset
  bag               Convert latest dataset to ROS bag
  camera            Run stereo camera calibration
  save-camera       Install latest camera-only result into target profile directory
  imu-camera        Run camera-IMU calibration
  convert-runtime   Convert final Kalibr camchain to Basalt/Mercury JSON
USAGE
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --target)
      [[ $# -ge 2 ]] || { usage >&2; exit 2; }
      TARGET="$2"
      shift 2
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *) break ;;
  esac
done

COMMAND="${1:-}"
[[ -n "$COMMAND" ]] || { usage >&2; exit 2; }
shift || true

export CALIB_TARGET="$TARGET"
case "$COMMAND" in
  show-target)
    # shellcheck source=scripts/load_target.sh
    source "$SCRIPT_DIR/scripts/load_target.sh"
    print_target_summary
    ;;
  install) exec "$SCRIPT_DIR/scripts/install_kalibr.sh" "$@" ;;
  record) exec "$SCRIPT_DIR/scripts/start_record.sh" "$@" ;;
  bag) exec "$SCRIPT_DIR/scripts/run_conversion_to_ros.sh" "$@" ;;
  camera) exec "$SCRIPT_DIR/scripts/run_kalibr_camera.sh" "$@" ;;
  save-camera) exec "$SCRIPT_DIR/scripts/save_camera_calibration.sh" "$@" ;;
  imu-camera) exec "$SCRIPT_DIR/scripts/run_kalibr_imu_camera.sh" "$@" ;;
  convert-runtime) exec "$SCRIPT_DIR/scripts/convert_runtime_json.sh" "$@" ;;
  *) echo "Unknown command: $COMMAND" >&2; usage >&2; exit 2 ;;
esac
