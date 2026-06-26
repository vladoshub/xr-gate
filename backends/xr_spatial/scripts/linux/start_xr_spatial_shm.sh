#!/usr/bin/env bash
set -euo pipefail

# Live SHM launcher for xr_spatial_backend.
# Intended for the SteamVR spatial overlay pipeline:
#   capture_service SHM + optional hmd_pose SHM
#     -> xr_spatial_backend
#     -> spatial_proxy_mesh SHM / runtime_spatial_summary SHM
#     -> xr_runtime_adapter / overlay tooling

expand_tilde_local() {
  local value="${1:-}"
  case "$value" in
    "~") printf '%s\n' "$HOME" ;;
    "~/"*) printf '%s\n' "$HOME/${value#"~/"}" ;;
    *) printf '%s\n' "$value" ;;
  esac
}

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_PROJECT="${ROOT_PROJECT:-${XR:-$HOME/src/xr_tracking}}"
ROOT_PROJECT="$(expand_tilde_local "$ROOT_PROJECT")"

# Runtime/live mode defaults. Override any of these from the environment.
XR_SPATIAL_PROFILE="${XR_SPATIAL_PROFILE:-${SPATIAL_MAPPER_PROFILE:-xreal_air2ultra_unified_480}}"
SPATIAL_MAPPER_PROFILE="$XR_SPATIAL_PROFILE"
XR_SPATIAL_MODE="${XR_SPATIAL_MODE:-${SPATIAL_MAPPER_MODE:-runtime}}"
SPATIAL_MAPPER_MODE="$XR_SPATIAL_MODE"
MAPPER_BACKEND="${MAPPER_BACKEND:-live_depth_grid}"

# SHM input/output defaults.
CAPTURE_REGISTRY="${CAPTURE_REGISTRY:-/tmp/capture_service_streams.json}"
SPATIAL_POSE_INPUT="${SPATIAL_POSE_INPUT:-${POSE_INPUT:-shm}}"
POSE_REGISTRY="${POSE_REGISTRY:-/tmp/tracking_streams.json}"
POSE_STREAM="${POSE_STREAM:-hmd_pose}"
POSE_WAIT_TIMEOUT_SEC="${POSE_WAIT_TIMEOUT_SEC:-0}"
POSE_RETRY_INTERVAL_MS="${POSE_RETRY_INTERVAL_MS:-500}"
POSE_REATTACH_ON_STALE_MS="${POSE_REATTACH_ON_STALE_MS:-500}"
RUNTIME_REGISTRY="${RUNTIME_REGISTRY:-/tmp/runtime_tracking_streams.json}"

PUBLISH_RUNTIME_SPATIAL_SHM="${PUBLISH_RUNTIME_SPATIAL_SHM:-1}"
SPATIAL_STREAM="${SPATIAL_STREAM:-runtime_spatial_summary}"
SPATIAL_SHM_NAME="${SPATIAL_SHM_NAME:-runtime_spatial_summary}"

SPATIAL_PROXY_MESH_ENABLED="${SPATIAL_PROXY_MESH_ENABLED:-1}"
PUBLISH_SPATIAL_PROXY_MESH_SHM="${PUBLISH_SPATIAL_PROXY_MESH_SHM:-1}"
SPATIAL_PROXY_MESH_STREAM="${SPATIAL_PROXY_MESH_STREAM:-spatial_proxy_mesh}"
SPATIAL_PROXY_MESH_SHM_NAME="${SPATIAL_PROXY_MESH_SHM_NAME:-spatial_proxy_mesh}"
SPATIAL_PROXY_MESH_RATE_HZ="${SPATIAL_PROXY_MESH_RATE_HZ:-30}"
SPATIAL_PROXY_MESH_VOXEL_SIZE_M="${SPATIAL_PROXY_MESH_VOXEL_SIZE_M:-0.08}"
SPATIAL_PROXY_MESH_MAX_VERTICES="${SPATIAL_PROXY_MESH_MAX_VERTICES:-8192}"
SPATIAL_PROXY_MESH_MAX_TRIANGLES="${SPATIAL_PROXY_MESH_MAX_TRIANGLES:-12000}"

# Runtime mode should not spam scan artifacts by default.
SAVE_DEBUG_FRAMES="${SAVE_DEBUG_FRAMES:-0}"
SAVE_DEBUG_CLOUDS="${SAVE_DEBUG_CLOUDS:-0}"
QUALITY_WRITE_CSV="${QUALITY_WRITE_CSV:-0}"
QUALITY_WRITE_POSE_HEALTH_CSV="${QUALITY_WRITE_POSE_HEALTH_CSV:-0}"
PRINT_EVERY="${PRINT_EVERY:-30}"

# Optional launcher log. Example: LOG_FILE=/tmp/xr_spatial_backend_shm.log ./start_xr_spatial_shm.sh
LOG_FILE="${LOG_FILE:-}"
if [[ -n "$LOG_FILE" ]]; then
  LOG_FILE="$(expand_tilde_local "$LOG_FILE")"
  mkdir -p "$(dirname "$LOG_FILE")"
  exec > >(tee -a "$LOG_FILE") 2>&1
fi

PROFILE_LOADER=""
if [[ -f "$SCRIPT_DIR/xr_spatial_profile.sh" ]]; then
  PROFILE_LOADER="$SCRIPT_DIR/xr_spatial_profile.sh"
elif [[ -f "$ROOT_PROJECT/backends/xr_spatial/scripts/linux/xr_spatial_profile.sh" ]]; then
  PROFILE_LOADER="$ROOT_PROJECT/backends/xr_spatial/scripts/linux/xr_spatial_profile.sh"
elif [[ -f "$ROOT_PROJECT/bin/backends/xr_spatial/scripts/linux/xr_spatial_profile.sh" ]]; then
  PROFILE_LOADER="$ROOT_PROJECT/bin/backends/xr_spatial/scripts/linux/xr_spatial_profile.sh"
else
  echo "[start_xr_spatial_shm][ERROR] xr_spatial_profile.sh not found." >&2
  echo "[start_xr_spatial_shm][ERROR] ROOT_PROJECT=$ROOT_PROJECT" >&2
  exit 2
fi

# shellcheck source=/dev/null
source "$PROFILE_LOADER"

load_xr_spatial_profile "start_xr_spatial_shm"
require_xr_spatial_calibration "start_xr_spatial_shm"

XR_SPATIAL_BIN="${XR_SPATIAL_BIN:-${SPATIAL_MAPPER_BIN:-$BIN_DIR/xr_spatial_backend}}"
SPATIAL_MAPPER_BIN="$XR_SPATIAL_BIN"
SPATIAL_MAPPER_BIN="$(expand_tilde "$SPATIAL_MAPPER_BIN")"

if [[ ! -x "$SPATIAL_MAPPER_BIN" ]]; then
  echo "[start_xr_spatial_shm][ERROR] xr_spatial_backend binary not found or not executable: $SPATIAL_MAPPER_BIN" >&2
  echo "[start_xr_spatial_shm][ERROR] Build it first:" >&2
  echo "  $ROOT_PROJECT/backends/xr_spatial/scripts/linux/install_xr_spatial.sh" >&2
  exit 1
fi

if [[ "$SPATIAL_MAPPER_MODE" != "runtime" && "$SPATIAL_MAPPER_MODE" != "scan" ]]; then
  echo "[start_xr_spatial_shm][ERROR] SPATIAL_MAPPER_MODE must be runtime or scan, got: $SPATIAL_MAPPER_MODE" >&2
  exit 2
fi

if [[ "$SPATIAL_MAPPER_MODE" == "scan" ]]; then
  mkdir -p "$SCAN_OUTPUT_DIR"
fi

echo "== xr_spatial_backend / SHM =="
echo "root: $ROOT_PROJECT"
echo "profile: ${SPATIAL_MAPPER_PROFILE_NAME:-$SPATIAL_MAPPER_PROFILE}"
echo "config: $SPATIAL_MAPPER_CONFIG"
echo "binary: $SPATIAL_MAPPER_BIN"
echo "mode: $SPATIAL_MAPPER_MODE"
echo "calib: $CALIB_JSON"
echo "capture: $CAPTURE_REGISTRY streams=$CAMERA0_STREAM,$CAMERA1_STREAM,$IMU_STREAM"
echo "pose: input=$SPATIAL_POSE_INPUT registry=$POSE_REGISTRY stream=$POSE_STREAM wait=${POSE_WAIT_TIMEOUT_SEC}s retry=${POSE_RETRY_INTERVAL_MS}ms max_age=${MAX_POSE_AGE_MS}ms reattach_stale=${POSE_REATTACH_ON_STALE_MS}ms"
echo "runtime_registry: $RUNTIME_REGISTRY"
echo "summary_shm: enabled=$PUBLISH_RUNTIME_SPATIAL_SHM stream=$SPATIAL_STREAM shm=$SPATIAL_SHM_NAME"
echo "proxy_mesh_shm: enabled=$SPATIAL_PROXY_MESH_ENABLED publish=$PUBLISH_SPATIAL_PROXY_MESH_SHM stream=$SPATIAL_PROXY_MESH_STREAM shm=$SPATIAL_PROXY_MESH_SHM_NAME rate=${SPATIAL_PROXY_MESH_RATE_HZ}Hz triangles=$SPATIAL_LIVE_GRID_TRIANGLES_ENABLED max_triangles=$SPATIAL_PROXY_MESH_MAX_TRIANGLES"
echo "mapper: $MAPPER_BACKEND depth=${DEPTH_RATE_HZ}Hz range=${MIN_DEPTH_M}-${MAX_DEPTH_M} voxel=$POINT_VOXEL_SIZE_M max_points_frame=$MAX_POINTS_PER_FRAME"
echo "quality: enabled=$QUALITY_GATE_ENABLED min_raw=$QUALITY_MIN_RAW_POINTS min_filtered=$QUALITY_MIN_FILTERED_POINTS pose_delta=$QUALITY_MAX_POSE_DELTA_M/$QUALITY_MAX_POSE_DELTA_DEG"

args=(
  --mode "$SPATIAL_MAPPER_MODE"
  --capture-registry "$CAPTURE_REGISTRY"
  --camera0-stream "$CAMERA0_STREAM"
  --camera1-stream "$CAMERA1_STREAM"
  --imu-stream "$IMU_STREAM"
  --pose-input "$SPATIAL_POSE_INPUT"
  --pose-registry "$POSE_REGISTRY"
  --pose-stream "$POSE_STREAM"
  --pose-frame-id "$POSE_FRAME_ID"
  --pose-wait-timeout-sec "$POSE_WAIT_TIMEOUT_SEC"
  --pose-retry-interval-ms "$POSE_RETRY_INTERVAL_MS"
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
[[ "${QUALITY_WRITE_CSV:-0}" == "0" ]] && args+=(--no-quality-write-csv) || args+=(--quality-write-csv)
[[ "${QUALITY_WRITE_POSE_HEALTH_CSV:-0}" == "0" ]] && args+=(--no-quality-write-pose-health-csv) || args+=(--quality-write-pose-health-csv)
[[ "${ZERO_DISPARITY:-1}" == "0" ]] && args+=(--no-zero-disparity) || args+=(--zero-disparity)
[[ "${PUBLISH_RUNTIME_SPATIAL_SHM:-1}" == "0" ]] || args+=(--publish-runtime-spatial-shm)
[[ "${SPATIAL_PROXY_MESH_ENABLED:-1}" == "0" ]] && args+=(--no-spatial-proxy-mesh-enabled) || args+=(--spatial-proxy-mesh-enabled)
[[ "${SPATIAL_LIVE_GRID_TRIANGLES_ENABLED:-1}" == "0" ]] && args+=(--no-spatial-live-grid-triangles-enabled) || args+=(--spatial-live-grid-triangles-enabled)
[[ "${PUBLISH_SPATIAL_PROXY_MESH_SHM:-1}" == "0" ]] || args+=(--publish-spatial-proxy-mesh-shm)
[[ "${SAVE_DEBUG_CLOUDS:-0}" == "0" ]] && args+=(--no-save-debug-clouds) || args+=(--save-debug-clouds)
[[ "${SAVE_DEBUG_FRAMES:-0}" == "0" ]] || args+=(--save-debug-frames --debug-dir "${DEBUG_DIR:-/tmp/xr_spatial_backend_debug}")

# scan-only options are accepted but only meaningful in SPATIAL_MAPPER_MODE=scan.
if [[ "$SPATIAL_MAPPER_MODE" == "scan" ]]; then
  args+=(
    --scan-duration-sec "$SCAN_DURATION_SEC"
    --scan-output-dir "$SCAN_OUTPUT_DIR"
  )
  [[ "${RESET_MAP_ON_SCAN_START:-1}" == "0" ]] && args+=(--no-reset-map-on-scan-start)
  [[ "${SAVE_POINTCLOUD_PLY:-1}" == "0" ]] && args+=(--no-save-pointcloud-ply) || args+=(--save-pointcloud-ply)
  [[ "${SAVE_TRAJECTORY_CSV:-1}" == "0" ]] && args+=(--no-save-trajectory-csv) || args+=(--save-trajectory-csv)
  [[ "${SAVE_METADATA_JSON:-1}" == "0" ]] && args+=(--no-save-metadata-json) || args+=(--save-metadata-json)
  [[ "${EXIT_AFTER_SCAN:-1}" == "0" ]] || args+=(--exit-after-scan)
fi

exec "$SPATIAL_MAPPER_BIN" "${args[@]}" "$@"
