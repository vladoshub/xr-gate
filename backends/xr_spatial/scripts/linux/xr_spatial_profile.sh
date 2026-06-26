#!/usr/bin/env bash
# Shared profile/config loader for xr_spatial launch scripts.
# Keep this script device-neutral: camera names, calibration paths, frame IDs and
# depth tuning belong in config profiles under backends/xr_spatial/configs/profiles/.

# XR_SPATIAL_BACKEND_BIN_LAYOUT_BEGIN
# Detect portable bin layout when this script is copied to:
#   bin/backends/xr_spatial/scripts/linux/xr_spatial_profile.sh
_XR_SPATIAL_PROFILE_SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
_XR_SPATIAL_DETECTED_APP_HOME=""
case "$_XR_SPATIAL_PROFILE_SCRIPT_DIR" in
  */bin/backends/xr_spatial/scripts/linux)
    _XR_SPATIAL_DETECTED_APP_HOME="$(cd "$_XR_SPATIAL_PROFILE_SCRIPT_DIR/../.." && pwd)"
    ;;
esac
# XR_SPATIAL_BACKEND_BIN_LAYOUT_END


expand_tilde() {
  local value="${1:-}"
  case "$value" in
    "~") printf '%s\n' "$HOME" ;;
    "~/"*) printf '%s\n' "$HOME/${value#"~/"}" ;;
    *) printf '%s\n' "$value" ;;
  esac
}

set_default() {
  local name="$1"
  local value="$2"
  if [[ -z "${!name:-}" ]]; then
    printf -v "$name" '%s' "$value"
  fi
}

resolve_xr_spatial_config() {
  local root_project="$1"
  if [[ -n "${XR_SPATIAL_CONFIG:-}" ]]; then
    expand_tilde "$XR_SPATIAL_CONFIG"
    return 0
  fi
  if [[ -n "${SPATIAL_MAPPER_CONFIG:-}" ]]; then
    expand_tilde "$SPATIAL_MAPPER_CONFIG"
    return 0
  fi

  local profile="${XR_SPATIAL_PROFILE:-${SPATIAL_MAPPER_PROFILE:-reference}}"
  printf '%s\n' "$root_project/backends/xr_spatial/configs/profiles/$profile.env"
}

load_xr_spatial_profile() {
  local log_prefix="$1"
  local scan_launch="0"
  if [[ "$log_prefix" == "start_xr_spatial_scan" || "$log_prefix" == "start_spatial_scan" ]]; then
    scan_launch="1"
  fi

  XR_SPATIAL_APP_HOME="${XR_SPATIAL_APP_HOME:-${SPATIAL_MAPPER_APP_HOME:-$_XR_SPATIAL_DETECTED_APP_HOME}}"
  if [[ -n "$XR_SPATIAL_APP_HOME" && -z "${ROOT_PROJECT:-}" && -z "${XR:-}" ]]; then
    ROOT_PROJECT="$XR_SPATIAL_APP_HOME"
  else
    ROOT_PROJECT="${ROOT_PROJECT:-${XR:-$HOME/src/xr_tracking}}"
  fi
  ROOT_PROJECT="$(expand_tilde "$ROOT_PROJECT")"

  XR_SPATIAL_CONFIG_RESOLVED="$(resolve_xr_spatial_config "$ROOT_PROJECT")"
  XR_SPATIAL_CONFIG_RESOLVED="$(expand_tilde "$XR_SPATIAL_CONFIG_RESOLVED")"
  # Legacy variable exported for old wrappers and existing diagnostics.
  SPATIAL_MAPPER_CONFIG="$XR_SPATIAL_CONFIG_RESOLVED"
  if [[ ! -f "$XR_SPATIAL_CONFIG_RESOLVED" ]]; then
    echo "[$log_prefix][ERROR] xr_spatial config not found: $XR_SPATIAL_CONFIG_RESOLVED" >&2
    echo "[$log_prefix][ERROR] set XR_SPATIAL_PROFILE=<name> or XR_SPATIAL_CONFIG=/path/to/profile.env" >&2
    echo "[$log_prefix][ERROR] legacy SPATIAL_MAPPER_PROFILE/SPATIAL_MAPPER_CONFIG are still accepted" >&2
    exit 2
  fi

  # shellcheck source=/dev/null
  source "$XR_SPATIAL_CONFIG_RESOLVED"

  if [[ -n "${XR_SPATIAL_APP_HOME:-}" && "$ROOT_PROJECT" == "$XR_SPATIAL_APP_HOME" ]]; then
    set_default BIN_DIR "$XR_SPATIAL_APP_HOME"
  else
    set_default BIN_DIR "$ROOT_PROJECT/bin/backends/xr_spatial"
  fi
  set_default CAPTURE_REGISTRY "/tmp/capture_service_streams.json"
  set_default CAMERA0_STREAM "camera0"
  set_default CAMERA1_STREAM "camera1"
  set_default IMU_STREAM "imu0"

  set_default SPATIAL_POSE_INPUT "${POSE_INPUT:-shm}"
  set_default POSE_REGISTRY "/tmp/tracking_streams.json"
  set_default POSE_STREAM "hmd_pose"
  set_default MAX_POSE_AGE_MS "120"
  set_default POSE_REATTACH_ON_STALE_MS "500"
  set_default STEREO_POSE_SYNC_SCAN_BACK "16"
  set_default DROP_STALE_POSE "1"

  set_default FISHEYE_BALANCE "0.0"
  set_default ZERO_DISPARITY "1"
  set_default DEPTH_RATE_HZ "10"
  set_default NUM_DISPARITIES "128"
  set_default BLOCK_SIZE "7"
  set_default UNIQUENESS_RATIO "10"
  set_default SPECKLE_WINDOW_SIZE "100"
  set_default SPECKLE_RANGE "2"
  set_default POINT_DECIMATION "2"
  set_default MIN_DEPTH_M "0.25"
  set_default MAX_DEPTH_M "2.5"
  set_default MAX_ABS_CAMERA_COORDINATE_M "20.0"
  set_default MAX_ACCUMULATED_POINTS "1000000"
  set_default MAX_ABS_MAP_COORDINATE_M "100.0"
  set_default TRACKING_ORIGIN "first_pose"
  if [[ "$scan_launch" == "1" ]]; then
    set_default MAPPER_BACKEND "pointcloud_fallback"
  else
    set_default MAPPER_BACKEND "live_depth_grid"
  fi
  set_default POINT_VOXEL_SIZE_M "0.01"
  set_default MAX_POINTS_PER_FRAME "60000"
  set_default DEPTH_ROI_X_MIN "0.0"
  set_default DEPTH_ROI_X_MAX "1.0"
  set_default DEPTH_ROI_Y_MIN "0.0"
  set_default DEPTH_ROI_Y_MAX "1.0"
  set_default QUALITY_GATE_ENABLED "1"
  set_default QUALITY_MIN_RAW_POINTS "1000"
  set_default QUALITY_MIN_FILTERED_POINTS "500"
  set_default QUALITY_FAR_DEPTH_M "0"
  set_default QUALITY_MAX_FAR_POINT_FRACTION "1.0"
  set_default QUALITY_MAX_POSE_DELTA_M "0"
  set_default QUALITY_MAX_POSE_DELTA_DEG "0"
  set_default QUALITY_REINIT_DELTA_M "1.0"
  set_default QUALITY_REINIT_DELTA_DEG "45.0"
  set_default QUALITY_WRITE_POSE_HEALTH_CSV "1"
  set_default QUALITY_POSE_HEALTH_CSV_NAME "pose_health.csv"
  set_default QUALITY_WRITE_CSV "1"
  set_default QUALITY_CSV_NAME "quality_gate.csv"


  set_default DEPTH_FRAME_ID "cam0_rect"
  set_default POSE_FRAME_ID "tracking_pose"
  if [[ "$SPATIAL_POSE_INPUT" == "none" ]]; then
    set_default SPATIAL_MAP_FRAME "camera"
    set_default MAP_FRAME_ID "$DEPTH_FRAME_ID"
  else
    set_default SPATIAL_MAP_FRAME "tracking"
    set_default MAP_FRAME_ID "tracking_world"
  fi
  set_default CALIB_PROFILE_NAME "unknown"
  set_default SAVE_DEBUG_CLOUDS "1"
  set_default SAVE_DEBUG_FRAMES "1"

  if [[ "$scan_launch" == "1" ]]; then
    set_default PUBLISH_RUNTIME_SPATIAL_SHM "0"
  else
    set_default PUBLISH_RUNTIME_SPATIAL_SHM "1"
  fi
  set_default RUNTIME_REGISTRY "/tmp/runtime_tracking_streams.json"
  set_default SPATIAL_STREAM "runtime_spatial_summary"
  set_default SPATIAL_SHM_NAME "runtime_spatial_summary"
  set_default SPATIAL_PROXY_MESH_ENABLED "1"
  if [[ "$scan_launch" == "1" ]]; then
    set_default PUBLISH_SPATIAL_PROXY_MESH_SHM "0"
  else
    set_default PUBLISH_SPATIAL_PROXY_MESH_SHM "1"
  fi
  set_default SPATIAL_PROXY_MESH_STREAM "spatial_proxy_mesh"
  set_default SPATIAL_PROXY_MESH_SHM_NAME "spatial_proxy_mesh"
  set_default SPATIAL_PROXY_MESH_RATE_HZ "2"
  set_default SPATIAL_PROXY_MESH_VOXEL_SIZE_M "0.08"
  set_default SPATIAL_PROXY_MESH_MAX_DISTANCE_M "2.5"
  set_default SPATIAL_PROXY_MESH_MAX_VERTICES "8192"
  set_default SPATIAL_PROXY_MESH_MAX_TRIANGLES "12000"
  set_default SPATIAL_PROXY_MESH_MIN_POINTS_PER_VOXEL "1"
  set_default SPATIAL_LIVE_GRID_TRIANGLES_ENABLED "1"
  set_default SPATIAL_LIVE_GRID_MAX_EDGE_M "0.10"
  set_default SPATIAL_LIVE_GRID_MAX_DEPTH_JUMP_M "0.10"

  set_default SCAN_DURATION_SEC "30"
  set_default SCAN_OUTPUT_ROOT "$HOME/xr_spatial_scans"
  set_default SCAN_OUTPUT_DIR "$SCAN_OUTPUT_ROOT/scan_$(date +%Y%m%d_%H%M%S)"
  set_default RESET_MAP_ON_SCAN_START "1"
  set_default EXIT_AFTER_SCAN "1"
  set_default SAVE_POINTCLOUD_PLY "1"
  set_default SAVE_VOXEL_POINTCLOUD_PLY "1"
  set_default SCAN_VOXEL_SIZE_M "0.02"
  set_default SCAN_MIN_OBSERVATIONS "2"
  set_default SCAN_MAX_VOXEL_POINTS "200000"
  set_default SAVE_TRAJECTORY_CSV "1"
  set_default SAVE_METADATA_JSON "1"
  set_default PRINT_EVERY "30"

  BIN_DIR="$(expand_tilde "$BIN_DIR")"
  CAPTURE_REGISTRY="$(expand_tilde "$CAPTURE_REGISTRY")"
  POSE_REGISTRY="$(expand_tilde "$POSE_REGISTRY")"
  RUNTIME_REGISTRY="$(expand_tilde "$RUNTIME_REGISTRY")"
  case "$SPATIAL_POSE_INPUT" in
    shm|none) ;;
    *)
      echo "[$log_prefix][ERROR] SPATIAL_POSE_INPUT must be shm or none, got: $SPATIAL_POSE_INPUT" >&2
      exit 2
      ;;
  esac
  CALIB_JSON="$(expand_tilde "${CALIB_JSON:-}")"
  SCAN_OUTPUT_ROOT="$(expand_tilde "$SCAN_OUTPUT_ROOT")"
  SCAN_OUTPUT_DIR="$(expand_tilde "$SCAN_OUTPUT_DIR")"

  APP_LIB_DIR="${APP_LIB_DIR:-$BIN_DIR/lib}"
  APP_LIB_DIR="$(expand_tilde "$APP_LIB_DIR")"
  if [[ -d "$APP_LIB_DIR" ]]; then
    export LD_LIBRARY_PATH="$APP_LIB_DIR:${LD_LIBRARY_PATH:-}"
  fi
}

require_xr_spatial_calibration() {
  local log_prefix="$1"
  if [[ -z "${CALIB_JSON:-}" ]]; then
    echo "[$log_prefix][ERROR] CALIB_JSON is not set." >&2
    echo "[$log_prefix][ERROR] Put the calibration JSON path in a profile under:" >&2
    echo "[$log_prefix][ERROR]   $ROOT_PROJECT/backends/xr_spatial/configs/profiles/" >&2
    echo "[$log_prefix][ERROR] or pass CALIB_JSON=/path/to/calib.json." >&2
    exit 2
  fi
  if [[ ! -f "$CALIB_JSON" ]]; then
    echo "[$log_prefix][ERROR] CALIB_JSON does not exist: $CALIB_JSON" >&2
    exit 2
  fi
}

# Legacy function aliases for old wrappers/scripts.
load_spatial_mapper_profile() { load_xr_spatial_profile "$@"; }
require_spatial_mapper_calibration() { require_xr_spatial_calibration "$@"; }
