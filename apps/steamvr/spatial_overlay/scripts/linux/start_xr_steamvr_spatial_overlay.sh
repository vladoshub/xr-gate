#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
if [[ -x "$SCRIPT_DIR/../../xr_steamvr_spatial_overlay" ]]; then
  INSTALL_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
  ROOT_PROJECT="${ROOT_PROJECT:-$(cd "$INSTALL_DIR/../../../.." && pwd)}"
  APP_BIN="$INSTALL_DIR/xr_steamvr_spatial_overlay"
  DEFAULT_CONFIG="$INSTALL_DIR/configs/profiles/runtime_shm.env"
else
  APP_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
  ROOT_PROJECT="${ROOT_PROJECT:-$(cd "$APP_DIR/../../.." && pwd)}"
  APP_BIN="${APP_BIN:-$ROOT_PROJECT/out/xrea_ultra/bin/apps/steamvr/spatial_overlay/xr_steamvr_spatial_overlay}"
  DEFAULT_CONFIG="$APP_DIR/configs/profiles/runtime_shm.env"
fi

SPATIAL_OVERLAY_PROFILE="${SPATIAL_OVERLAY_PROFILE:-runtime_shm}"
CONFIG="${SPATIAL_OVERLAY_CONFIG:-$DEFAULT_CONFIG}"
if [[ -f "$CONFIG" ]]; then
  # shellcheck disable=SC1090
  source "$CONFIG"
fi

: "${SPATIAL_OVERLAY_REGISTRY:=/tmp/runtime_tracking_streams.json}"
: "${SPATIAL_OVERLAY_STREAM:=runtime_spatial_proxy_mesh}"
: "${SPATIAL_OVERLAY_LOG_FILE:=/tmp/xr_steamvr_spatial_overlay.log}"
: "${SPATIAL_OVERLAY_WIDTH:=1280}"
: "${SPATIAL_OVERLAY_HEIGHT:=720}"
: "${SPATIAL_OVERLAY_RENDER_HZ:=60}"
: "${SPATIAL_OVERLAY_STEREO_SBS:=0}"
: "${SPATIAL_OVERLAY_STEREO_IPD_M:=0.064}"
: "${SPATIAL_OVERLAY_STEREO_OPENVR_FLAG:=1}"
: "${SPATIAL_OVERLAY_STEREO_DRAW_HUD:=0}"
: "${SPATIAL_OVERLAY_DRY_RUN:=0}"
: "${SPATIAL_OVERLAY_VERBOSE_FRAMES:=0}"
: "${SPATIAL_OVERLAY_TEXTURE_FLIP_Y:=1}"
: "${SPATIAL_OVERLAY_TEXTURE_FLIP_X:=0}"
: "${SPATIAL_OVERLAY_TEXTURE_ROTATE_180:=0}"
: "${SPATIAL_OVERLAY_DRAW_DISTANCE_MARKERS:=1}"
: "${SPATIAL_OVERLAY_COLOR_BY_DISTANCE:=1}"
: "${SPATIAL_OVERLAY_DISTANCE_MARKER_STEP_M:=0.5}"
: "${SPATIAL_OVERLAY_DISTANCE_MARKER_MAX_M:=3.0}"
: "${SPATIAL_OVERLAY_DRAW_MESH:=1}"
: "${SPATIAL_OVERLAY_DRAW_WIRE:=0}"
: "${SPATIAL_OVERLAY_DRAW_POINTS:=0}"
: "${SPATIAL_OVERLAY_DRAW_BBOX:=0}"
: "${SPATIAL_OVERLAY_POINT_RADIUS_PX:=2}"
: "${SPATIAL_OVERLAY_MESH_ALPHA:=0.38}"
: "${SPATIAL_OVERLAY_WIRE_ALPHA:=0.82}"
: "${SPATIAL_OVERLAY_POINT_ALPHA:=0.85}"
: "${SPATIAL_OVERLAY_SUBMIT_MODE:=opengl}"
: "${SPATIAL_OVERLAY_FORCE_VISIBLE_BORDER:=0}"
: "${SPATIAL_OVERLAY_TEST_PATTERN:=0}"
: "${SPATIAL_OVERLAY_DRAW_STALE_MESH:=1}"
: "${SPATIAL_OVERLAY_ALPHA:=0.75}"
: "${SPATIAL_OVERLAY_VERTICAL_FOV_DEG:=75}"
: "${SPATIAL_OVERLAY_MIN_DEPTH_M:=0.05}"
: "${SPATIAL_OVERLAY_MAX_DEPTH_M:=8.0}"
: "${SPATIAL_OVERLAY_MAX_MESH_AGE_MS:=3000}"

: "${SPATIAL_OVERLAY_POSE_SOURCE:=xr_runtime}"
: "${SPATIAL_OVERLAY_POSE_REGISTRY:=/tmp/runtime_tracking_streams.json}"
: "${SPATIAL_OVERLAY_POSE_STREAM:=runtime_hmd_pose}"
: "${SPATIAL_OVERLAY_CLEAR_WHEN_NO_MESH:=1}"
: "${SPATIAL_OVERLAY_CLEAR_WHEN_NO_POSE:=1}"
: "${SPATIAL_OVERLAY_CLEAR_ON_STALE_SKIP:=1}"
: "${SPATIAL_OVERLAY_POSE_VERBOSE:=0}"

: "${SPATIAL_OVERLAY_AUTO_STEAMVR_ENV:=1}"
: "${SPATIAL_OVERLAY_STEAMVR_ROOT:=}"
: "${SPATIAL_OVERLAY_STEAM_ROOT:=}"
: "${SPATIAL_OVERLAY_DUMP_OPENVR_LDD:=1}"
: "${SPATIAL_OVERLAY_WAIT_STEAMVR:=0}"
: "${SPATIAL_OVERLAY_STEAMVR_WAIT_TIMEOUT_SEC:=20}"

first_existing_dir() {
  local p
  for p in "$@"; do
    [[ -n "$p" && -d "$p" ]] && { printf '%s\n' "$p"; return 0; }
  done
  return 1
}

append_ld_path() {
  local p="$1"
  [[ -n "$p" && -d "$p" ]] || return 0
  case ":${LD_LIBRARY_PATH:-}:" in
    *":$p:"*) ;;
    *) export LD_LIBRARY_PATH="$p${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" ;;
  esac
}

if [[ "$SPATIAL_OVERLAY_AUTO_STEAMVR_ENV" == "1" && "$SPATIAL_OVERLAY_DRY_RUN" != "1" ]]; then
  if [[ -z "$SPATIAL_OVERLAY_STEAMVR_ROOT" ]]; then
    SPATIAL_OVERLAY_STEAMVR_ROOT="$(first_existing_dir \
      "${STEAMVR_ROOT:-}" \
      "${VR_OVERRIDE:-}" \
      "$HOME/.local/share/Steam/steamapps/common/SteamVR" \
      "$HOME/.steam/steam/steamapps/common/SteamVR" \
      "$HOME/.steam/debian-installation/steamapps/common/SteamVR" \
      "$HOME/snap/steam/common/.local/share/Steam/steamapps/common/SteamVR" \
      2>/dev/null || true)"
  fi

  if [[ -z "$SPATIAL_OVERLAY_STEAM_ROOT" ]]; then
    SPATIAL_OVERLAY_STEAM_ROOT="$(first_existing_dir \
      "${STEAM_ROOT:-}" \
      "$HOME/.local/share/Steam" \
      "$HOME/.steam/steam" \
      "$HOME/.steam/debian-installation" \
      "$HOME/snap/steam/common/.local/share/Steam" \
      2>/dev/null || true)"
  fi

  APP_LIB_DIR="$(cd "$(dirname "$APP_BIN")/lib" 2>/dev/null && pwd || true)"
  append_ld_path "$APP_LIB_DIR"

  if [[ -n "$SPATIAL_OVERLAY_STEAMVR_ROOT" ]]; then
    export VR_OVERRIDE="${VR_OVERRIDE:-$SPATIAL_OVERLAY_STEAMVR_ROOT}"
    append_ld_path "$SPATIAL_OVERLAY_STEAMVR_ROOT/bin/linux64"
    append_ld_path "$SPATIAL_OVERLAY_STEAMVR_ROOT/lib/linux64"
    append_ld_path "$SPATIAL_OVERLAY_STEAMVR_ROOT/bin"
  fi

  if [[ -n "$SPATIAL_OVERLAY_STEAM_ROOT" ]]; then
    export VR_CONFIG_PATH="${VR_CONFIG_PATH:-$SPATIAL_OVERLAY_STEAM_ROOT/config}"
    export VR_LOG_PATH="${VR_LOG_PATH:-$SPATIAL_OVERLAY_STEAM_ROOT/logs}"
    append_ld_path "$SPATIAL_OVERLAY_STEAM_ROOT/ubuntu12_64"
    append_ld_path "$SPATIAL_OVERLAY_STEAM_ROOT/ubuntu12_32"
    append_ld_path "$SPATIAL_OVERLAY_STEAM_ROOT/linux64"
    append_ld_path "$SPATIAL_OVERLAY_STEAM_ROOT"
  fi
fi

if [[ "$SPATIAL_OVERLAY_WAIT_STEAMVR" == "1" && "$SPATIAL_OVERLAY_DRY_RUN" != "1" ]]; then
  deadline=$((SECONDS + SPATIAL_OVERLAY_STEAMVR_WAIT_TIMEOUT_SEC))
  while (( SECONDS < deadline )); do
    if pgrep -f 'vrserver|vrcompositor' >/dev/null 2>&1; then
      break
    fi
    echo "[start_xr_steamvr_spatial_overlay] waiting for SteamVR vrserver/vrcompositor..."
    sleep 1
  done
fi

if [[ ! -x "$APP_BIN" ]]; then
  echo "[start_xr_steamvr_spatial_overlay][ERROR] binary not found/executable: $APP_BIN" >&2
  echo "[start_xr_steamvr_spatial_overlay][ERROR] Run apps/steamvr/spatial_overlay/scripts/linux/install_xr_steamvr_spatial_overlay.sh" >&2
  exit 2
fi

mkdir -p "$(dirname "$SPATIAL_OVERLAY_LOG_FILE")"

echo "== xr_steamvr_spatial_overlay =="
echo "ROOT_PROJECT=$ROOT_PROJECT"
echo "APP_BIN=$APP_BIN"
echo "CONFIG=$CONFIG"
echo "registry=$SPATIAL_OVERLAY_REGISTRY stream=$SPATIAL_OVERLAY_STREAM"
echo "texture=${SPATIAL_OVERLAY_WIDTH}x${SPATIAL_OVERLAY_HEIGHT} render_hz=$SPATIAL_OVERLAY_RENDER_HZ dry_run=$SPATIAL_OVERLAY_DRY_RUN submit_mode=$SPATIAL_OVERLAY_SUBMIT_MODE distance_markers=$SPATIAL_OVERLAY_DRAW_DISTANCE_MARKERS"
echo "stereo_sbs=$SPATIAL_OVERLAY_STEREO_SBS ipd_m=$SPATIAL_OVERLAY_STEREO_IPD_M openvr_flag=$SPATIAL_OVERLAY_STEREO_OPENVR_FLAG stereo_hud=$SPATIAL_OVERLAY_STEREO_DRAW_HUD"
echo "render: mesh=$SPATIAL_OVERLAY_DRAW_MESH wire=$SPATIAL_OVERLAY_DRAW_WIRE points=$SPATIAL_OVERLAY_DRAW_POINTS bbox=$SPATIAL_OVERLAY_DRAW_BBOX point_radius_px=$SPATIAL_OVERLAY_POINT_RADIUS_PX alpha(mesh/wire/point)=$SPATIAL_OVERLAY_MESH_ALPHA/$SPATIAL_OVERLAY_WIRE_ALPHA/$SPATIAL_OVERLAY_POINT_ALPHA"
echo "log_file=$SPATIAL_OVERLAY_LOG_FILE"
echo "texture_flip=Y:$SPATIAL_OVERLAY_TEXTURE_FLIP_Y X:$SPATIAL_OVERLAY_TEXTURE_FLIP_X rotate180:$SPATIAL_OVERLAY_TEXTURE_ROTATE_180"
echo "pose_source=$SPATIAL_OVERLAY_POSE_SOURCE pose_registry=$SPATIAL_OVERLAY_POSE_REGISTRY pose_stream=$SPATIAL_OVERLAY_POSE_STREAM clear_no_mesh=$SPATIAL_OVERLAY_CLEAR_WHEN_NO_MESH clear_no_pose=$SPATIAL_OVERLAY_CLEAR_WHEN_NO_POSE"
echo "steamvr_root=${SPATIAL_OVERLAY_STEAMVR_ROOT:-<not-found>}"
echo "steam_root=${SPATIAL_OVERLAY_STEAM_ROOT:-<not-found>}"
echo "VR_OVERRIDE=${VR_OVERRIDE:-<unset>}"
echo "VR_CONFIG_PATH=${VR_CONFIG_PATH:-<unset>}"
echo "VR_LOG_PATH=${VR_LOG_PATH:-<unset>}"
echo "LD_LIBRARY_PATH=${LD_LIBRARY_PATH:-<unset>}"
if [[ "$SPATIAL_OVERLAY_DUMP_OPENVR_LDD" == "1" && "$SPATIAL_OVERLAY_DRY_RUN" != "1" ]]; then
  echo "[start_xr_steamvr_spatial_overlay] OpenVR process check:"
  pgrep -a -f 'vrserver|vrcompositor|vrmonitor' || true
  if [[ -n "${SPATIAL_OVERLAY_STEAMVR_ROOT:-}" && -x "$SPATIAL_OVERLAY_STEAMVR_ROOT/bin/linux64/vrmonitor" ]]; then
    echo "[start_xr_steamvr_spatial_overlay] ldd vrmonitor:"
    ldd "$SPATIAL_OVERLAY_STEAMVR_ROOT/bin/linux64/vrmonitor" 2>&1 | sed 's/^/[ldd vrmonitor] /' || true
  fi
  if [[ -n "${SPATIAL_OVERLAY_STEAMVR_ROOT:-}" ]]; then
    echo "[start_xr_steamvr_spatial_overlay] libsteam_api candidates:"
    find "$SPATIAL_OVERLAY_STEAMVR_ROOT" -maxdepth 4 -name 'libsteam_api.so' -printf '[libsteam_api] %p\n' 2>/dev/null || true
  fi
fi

export SPATIAL_OVERLAY_POSE_SOURCE
export SPATIAL_OVERLAY_POSE_REGISTRY
export SPATIAL_OVERLAY_POSE_STREAM
export SPATIAL_OVERLAY_CLEAR_WHEN_NO_MESH
export SPATIAL_OVERLAY_CLEAR_WHEN_NO_POSE
export SPATIAL_OVERLAY_CLEAR_ON_STALE_SKIP
export SPATIAL_OVERLAY_POSE_VERBOSE
export SPATIAL_OVERLAY_STEREO_SBS
export SPATIAL_OVERLAY_STEREO_IPD_M
export SPATIAL_OVERLAY_STEREO_OPENVR_FLAG
export SPATIAL_OVERLAY_STEREO_DRAW_HUD
export SPATIAL_OVERLAY_TEXTURE_FLIP_Y
export SPATIAL_OVERLAY_TEXTURE_FLIP_X
export SPATIAL_OVERLAY_TEXTURE_ROTATE_180
export SPATIAL_OVERLAY_DRAW_DISTANCE_MARKERS
export SPATIAL_OVERLAY_COLOR_BY_DISTANCE
export SPATIAL_OVERLAY_DISTANCE_MARKER_STEP_M
export SPATIAL_OVERLAY_DISTANCE_MARKER_MAX_M
export SPATIAL_OVERLAY_DRAW_MESH
export SPATIAL_OVERLAY_DRAW_WIRE
export SPATIAL_OVERLAY_DRAW_POINTS
export SPATIAL_OVERLAY_DRAW_BBOX
export SPATIAL_OVERLAY_POINT_RADIUS_PX
export SPATIAL_OVERLAY_MESH_ALPHA
export SPATIAL_OVERLAY_WIRE_ALPHA
export SPATIAL_OVERLAY_POINT_ALPHA
export SPATIAL_OVERLAY_FORCE_VISIBLE_BORDER
export SPATIAL_OVERLAY_TEST_PATTERN
export SPATIAL_OVERLAY_DRAW_STALE_MESH
export SPATIAL_OVERLAY_ALPHA
export SPATIAL_OVERLAY_VERTICAL_FOV_DEG
export SPATIAL_OVERLAY_MIN_DEPTH_M
export SPATIAL_OVERLAY_MAX_DEPTH_M
export SPATIAL_OVERLAY_MAX_MESH_AGE_MS

ARGS=(
  --registry "$SPATIAL_OVERLAY_REGISTRY"
  --stream "$SPATIAL_OVERLAY_STREAM"
  --log-file "$SPATIAL_OVERLAY_LOG_FILE"
  --pose-source "$SPATIAL_OVERLAY_POSE_SOURCE"
  --pose-registry "$SPATIAL_OVERLAY_POSE_REGISTRY"
  --pose-stream "$SPATIAL_OVERLAY_POSE_STREAM"
  --width "$SPATIAL_OVERLAY_WIDTH"
  --height "$SPATIAL_OVERLAY_HEIGHT"
  --render-hz "$SPATIAL_OVERLAY_RENDER_HZ"
)

if [[ "$SPATIAL_OVERLAY_STEREO_SBS" == "1" ]]; then
  ARGS+=(--stereo-sbs --stereo-ipd "$SPATIAL_OVERLAY_STEREO_IPD_M")
fi

if [[ "$SPATIAL_OVERLAY_DRY_RUN" == "1" ]]; then
  ARGS+=(--dry-run)
fi
if [[ "$SPATIAL_OVERLAY_VERBOSE_FRAMES" == "1" ]]; then
  ARGS+=(--verbose-frames)
fi

exec "$APP_BIN" "${ARGS[@]}" 2>&1 | tee -a "$SPATIAL_OVERLAY_LOG_FILE"
