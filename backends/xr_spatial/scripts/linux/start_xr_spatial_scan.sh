#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=xr_spatial_profile.sh
source "$SCRIPT_DIR/xr_spatial_profile.sh"

load_xr_spatial_profile "start_xr_spatial_scan"
require_xr_spatial_calibration "start_xr_spatial_scan"

# Scan is a backend-only feature by default: build artifacts on disk and do not
# feed runtime/driver streams unless explicitly requested for debugging.
MAPPER_BACKEND="${SCAN_MAPPER_BACKEND:-pointcloud_fallback}"
PUBLISH_RUNTIME_SPATIAL_SHM="${SCAN_PUBLISH_RUNTIME_SPATIAL_SHM:-0}"
PUBLISH_SPATIAL_PROXY_MESH_SHM="${SCAN_PUBLISH_SPATIAL_PROXY_MESH_SHM:-0}"

mkdir -p "$SCAN_OUTPUT_DIR"

if [[ "${SAVE_DEBUG_FRAMES:-1}" != "0" ]]; then
  DEBUG_DIR="${DEBUG_DIR:-$SCAN_OUTPUT_DIR/debug}"
  DEBUG_DIR="$(expand_tilde "$DEBUG_DIR")"
fi

echo "[start_xr_spatial_scan] ROOT_PROJECT=$ROOT_PROJECT"
echo "[start_xr_spatial_scan] CONFIG=$SPATIAL_MAPPER_CONFIG"
echo "[start_xr_spatial_scan] PROFILE=${SPATIAL_MAPPER_PROFILE_NAME:-${SPATIAL_MAPPER_PROFILE:-reference}}"
echo "[start_xr_spatial_scan] CALIB_JSON=$CALIB_JSON"
echo "[start_xr_spatial_scan] CALIB_PROFILE_NAME=$CALIB_PROFILE_NAME"
echo "[start_xr_spatial_scan] SCAN_OUTPUT_DIR=$SCAN_OUTPUT_DIR"
echo "[start_xr_spatial_scan] CAPTURE=$CAPTURE_REGISTRY streams=$CAMERA0_STREAM,$CAMERA1_STREAM,$IMU_STREAM"
echo "[start_xr_spatial_scan] POSE input=$SPATIAL_POSE_INPUT registry=$POSE_REGISTRY stream=$POSE_STREAM wait=${POSE_WAIT_TIMEOUT_SEC:-0}s retry=${POSE_RETRY_INTERVAL_MS:-500}ms reattach_stale=$POSE_REATTACH_ON_STALE_MS"
echo "[start_xr_spatial_scan] FRAMES depth=$DEPTH_FRAME_ID pose=$POSE_FRAME_ID map=$MAP_FRAME_ID"
echo "[start_xr_spatial_scan] DEPTH rate=$DEPTH_RATE_HZ disparities=$NUM_DISPARITIES block=$BLOCK_SIZE range=$MIN_DEPTH_M-$MAX_DEPTH_M map_frame=$SPATIAL_MAP_FRAME"
echo "[start_xr_spatial_scan] MAPPER=$MAPPER_BACKEND point_voxel=$POINT_VOXEL_SIZE_M max_points_frame=$MAX_POINTS_PER_FRAME"
echo "[start_xr_spatial_scan] PROXY_MESH enabled=$SPATIAL_PROXY_MESH_ENABLED stream=$SPATIAL_PROXY_MESH_STREAM voxel=$SPATIAL_PROXY_MESH_VOXEL_SIZE_M rate=$SPATIAL_PROXY_MESH_RATE_HZ max_vertices=$SPATIAL_PROXY_MESH_MAX_VERTICES max_triangles=$SPATIAL_PROXY_MESH_MAX_TRIANGLES"
echo "[start_xr_spatial_scan] DEPTH_ROI x=$DEPTH_ROI_X_MIN-$DEPTH_ROI_X_MAX y=$DEPTH_ROI_Y_MIN-$DEPTH_ROI_Y_MAX"
echo "[start_xr_spatial_scan] QUALITY_GATE enabled=$QUALITY_GATE_ENABLED min_raw=$QUALITY_MIN_RAW_POINTS min_filtered=$QUALITY_MIN_FILTERED_POINTS far=$QUALITY_FAR_DEPTH_M/$QUALITY_MAX_FAR_POINT_FRACTION pose_delta=$QUALITY_MAX_POSE_DELTA_M/$QUALITY_MAX_POSE_DELTA_DEG csv=$QUALITY_WRITE_CSV"
echo "[start_xr_spatial_scan] POSE_HEALTH stale_age_ms=$MAX_POSE_AGE_MS reinit=$QUALITY_REINIT_DELTA_M/$QUALITY_REINIT_DELTA_DEG csv=$QUALITY_WRITE_POSE_HEALTH_CSV"
echo "[start_xr_spatial_scan] BACKEND_ONLY runtime_spatial_shm=$PUBLISH_RUNTIME_SPATIAL_SHM proxy_mesh_shm=$PUBLISH_SPATIAL_PROXY_MESH_SHM voxel_ply=$SAVE_VOXEL_POINTCLOUD_PLY voxel_size=$SCAN_VOXEL_SIZE_M min_obs=$SCAN_MIN_OBSERVATIONS max_voxels=$SCAN_MAX_VOXEL_POINTS"

args=(
  --mode scan
  --capture-registry "$CAPTURE_REGISTRY"
  --camera0-stream "$CAMERA0_STREAM"
  --camera1-stream "$CAMERA1_STREAM"
  --imu-stream "$IMU_STREAM"
  --pose-input "$SPATIAL_POSE_INPUT"
  --pose-registry "$POSE_REGISTRY"
  --pose-stream "$POSE_STREAM"
  --pose-frame-id "$POSE_FRAME_ID"
  --pose-wait-timeout-sec "${POSE_WAIT_TIMEOUT_SEC:-0}"
  --pose-retry-interval-ms "${POSE_RETRY_INTERVAL_MS:-500}"
  --pose-reattach-on-stale-ms "$POSE_REATTACH_ON_STALE_MS"
  --max-pose-age-ms "$MAX_POSE_AGE_MS"
  --stereo-pose-sync-scan-back "$STEREO_POSE_SYNC_SCAN_BACK"
  --map-frame "$SPATIAL_MAP_FRAME"
  --map-frame-id "$MAP_FRAME_ID"
  --calib "$CALIB_JSON"
  --calib-profile-name "$CALIB_PROFILE_NAME"
  --depth-frame-id "$DEPTH_FRAME_ID"
  --fisheye-balance "$FISHEYE_BALANCE"
  --depth-rate-hz "$DEPTH_RATE_HZ"
  --num-disparities "$NUM_DISPARITIES"
  --block-size "$BLOCK_SIZE"
  --uniqueness-ratio "$UNIQUENESS_RATIO"
  --speckle-window-size "$SPECKLE_WINDOW_SIZE"
  --speckle-range "$SPECKLE_RANGE"
  --point-decimation "$POINT_DECIMATION"
  --min-depth-m "$MIN_DEPTH_M"
  --max-depth-m "$MAX_DEPTH_M"
  --max-abs-camera-coordinate-m "$MAX_ABS_CAMERA_COORDINATE_M"
  --depth-roi-x-min "$DEPTH_ROI_X_MIN"
  --depth-roi-x-max "$DEPTH_ROI_X_MAX"
  --depth-roi-y-min "$DEPTH_ROI_Y_MIN"
  --depth-roi-y-max "$DEPTH_ROI_Y_MAX"
  --max-accumulated-points "$MAX_ACCUMULATED_POINTS"
  --max-abs-map-coordinate-m "$MAX_ABS_MAP_COORDINATE_M"
  --tracking-origin "$TRACKING_ORIGIN"
  --mapper-backend "$MAPPER_BACKEND"
  --point-voxel-size-m "$POINT_VOXEL_SIZE_M"
  --max-points-per-frame "$MAX_POINTS_PER_FRAME"
  --quality-min-raw-points "$QUALITY_MIN_RAW_POINTS"
  --quality-min-filtered-points "$QUALITY_MIN_FILTERED_POINTS"
  --quality-far-depth-m "$QUALITY_FAR_DEPTH_M"
  --quality-max-far-point-fraction "$QUALITY_MAX_FAR_POINT_FRACTION"
  --quality-max-pose-delta-m "$QUALITY_MAX_POSE_DELTA_M"
  --quality-max-pose-delta-deg "$QUALITY_MAX_POSE_DELTA_DEG"
  --quality-reinit-delta-m "$QUALITY_REINIT_DELTA_M"
  --quality-reinit-delta-deg "$QUALITY_REINIT_DELTA_DEG"
  --quality-pose-health-csv-name "$QUALITY_POSE_HEALTH_CSV_NAME"
  --quality-csv-name "$QUALITY_CSV_NAME"
  --scan-duration-sec "$SCAN_DURATION_SEC"
  --scan-output-dir "$SCAN_OUTPUT_DIR"
  --scan-voxel-size-m "$SCAN_VOXEL_SIZE_M"
  --scan-min-observations "$SCAN_MIN_OBSERVATIONS"
  --scan-max-voxel-points "$SCAN_MAX_VOXEL_POINTS"
  --runtime-registry "$RUNTIME_REGISTRY"
  --spatial-stream "$SPATIAL_STREAM"
  --spatial-shm-name "$SPATIAL_SHM_NAME"
  --spatial-proxy-mesh-stream "$SPATIAL_PROXY_MESH_STREAM"
  --spatial-proxy-mesh-shm-name "$SPATIAL_PROXY_MESH_SHM_NAME"
  --spatial-proxy-mesh-rate-hz "$SPATIAL_PROXY_MESH_RATE_HZ"
  --spatial-proxy-mesh-voxel-size-m "$SPATIAL_PROXY_MESH_VOXEL_SIZE_M"
  --spatial-proxy-mesh-max-distance-m "$SPATIAL_PROXY_MESH_MAX_DISTANCE_M"
  --spatial-proxy-mesh-max-vertices "$SPATIAL_PROXY_MESH_MAX_VERTICES"
  --spatial-proxy-mesh-max-triangles "$SPATIAL_PROXY_MESH_MAX_TRIANGLES"
  --spatial-proxy-mesh-min-points-per-voxel "$SPATIAL_PROXY_MESH_MIN_POINTS_PER_VOXEL"
  --spatial-live-grid-max-edge-m "$SPATIAL_LIVE_GRID_MAX_EDGE_M"
  --spatial-live-grid-max-depth-jump-m "$SPATIAL_LIVE_GRID_MAX_DEPTH_JUMP_M"
  --print-every "$PRINT_EVERY"
)

[[ "${DROP_STALE_POSE:-1}" == "0" ]] && args+=(--no-drop-stale-pose)
[[ "${QUALITY_GATE_ENABLED:-1}" == "0" ]] && args+=(--no-quality-gate-enabled) || args+=(--quality-gate-enabled)
[[ "${QUALITY_WRITE_CSV:-1}" == "0" ]] && args+=(--no-quality-write-csv) || args+=(--quality-write-csv)
[[ "${QUALITY_WRITE_POSE_HEALTH_CSV:-1}" == "0" ]] && args+=(--no-quality-write-pose-health-csv) || args+=(--quality-write-pose-health-csv)
[[ "${ZERO_DISPARITY:-1}" == "0" ]] && args+=(--no-zero-disparity)
if [[ "${SAVE_DEBUG_FRAMES:-1}" != "0" ]]; then
  args+=(--save-debug-frames --debug-dir "$DEBUG_DIR")
fi
[[ "${SAVE_DEBUG_CLOUDS:-1}" == "0" ]] && args+=(--no-save-debug-clouds) || args+=(--save-debug-clouds)
[[ "${PUBLISH_RUNTIME_SPATIAL_SHM:-1}" == "0" ]] || args+=(--publish-runtime-spatial-shm)
[[ "${SPATIAL_PROXY_MESH_ENABLED:-1}" == "0" ]] && args+=(--no-spatial-proxy-mesh-enabled) || args+=(--spatial-proxy-mesh-enabled)
[[ "${SPATIAL_LIVE_GRID_TRIANGLES_ENABLED:-1}" == "0" ]] && args+=(--no-spatial-live-grid-triangles-enabled) || args+=(--spatial-live-grid-triangles-enabled)
[[ "${PUBLISH_SPATIAL_PROXY_MESH_SHM:-1}" == "0" ]] || args+=(--publish-spatial-proxy-mesh-shm)
[[ "${RESET_MAP_ON_SCAN_START:-1}" == "0" ]] && args+=(--no-reset-map-on-scan-start)
[[ "${SAVE_POINTCLOUD_PLY:-1}" == "0" ]] && args+=(--no-save-pointcloud-ply)
[[ "${SAVE_VOXEL_POINTCLOUD_PLY:-1}" == "0" ]] && args+=(--no-save-voxel-pointcloud-ply)
[[ "${SAVE_TRAJECTORY_CSV:-1}" == "0" ]] && args+=(--no-save-trajectory-csv)
[[ "${SAVE_METADATA_JSON:-1}" == "0" ]] && args+=(--no-save-metadata-json)
[[ "${EXIT_AFTER_SCAN:-1}" == "0" ]] || args+=(--exit-after-scan)

exec "$BIN_DIR/xr_spatial_backend" "${args[@]}"
