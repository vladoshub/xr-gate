#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
if [[ -x "$SCRIPT_DIR/../../xr_steamvr_spatial_scene" ]]; then
  INSTALL_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
  ROOT_PROJECT="${ROOT_PROJECT:-$(cd "$INSTALL_DIR/../../../.." && pwd)}"
  APP_BIN="$INSTALL_DIR/xr_steamvr_spatial_scene"
  DEFAULT_CONFIG="$INSTALL_DIR/configs/profiles/runtime_shm.env"
else
  APP_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
  ROOT_PROJECT="${ROOT_PROJECT:-$(cd "$APP_DIR/../../.." && pwd)}"
  APP_BIN="${APP_BIN:-$ROOT_PROJECT/out/xrea_ultra/bin/apps/steamvr/spatial_scene/xr_steamvr_spatial_scene}"
  DEFAULT_CONFIG="$APP_DIR/configs/profiles/runtime_shm.env"
fi

SPATIAL_SCENE_PROFILE="${SPATIAL_SCENE_PROFILE:-runtime_shm}"
CONFIG="${SPATIAL_SCENE_CONFIG:-$DEFAULT_CONFIG}"
if [[ -f "$CONFIG" ]]; then
  # shellcheck disable=SC1090
  source "$CONFIG"
fi

: "${SPATIAL_SCENE_REGISTRY:=/tmp/runtime_tracking_streams.json}"
: "${SPATIAL_SCENE_STREAM:=runtime_spatial_proxy_mesh}"
: "${SPATIAL_SCENE_LOG_FILE:=/tmp/xr_steamvr_spatial_scene.log}"
: "${SPATIAL_SCENE_WIDTH:=1280}"
: "${SPATIAL_SCENE_HEIGHT:=1280}"
: "${SPATIAL_SCENE_RENDER_HZ:=60}"
: "${SPATIAL_SCENE_DRY_RUN:=0}"
: "${SPATIAL_SCENE_VERBOSE_FRAMES:=0}"
: "${SPATIAL_SCENE_AUTO_STEAMVR_ENV:=1}"
: "${SPATIAL_SCENE_STEAMVR_ROOT:=}"
: "${SPATIAL_SCENE_STEAM_ROOT:=}"
: "${SPATIAL_SCENE_WAIT_STEAMVR:=0}"
: "${SPATIAL_SCENE_STEAMVR_WAIT_TIMEOUT_SEC:=20}"
: "${SPATIAL_SCENE_DUMP_OPENVR_LDD:=1}"

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

if [[ "$SPATIAL_SCENE_AUTO_STEAMVR_ENV" == "1" && "$SPATIAL_SCENE_DRY_RUN" != "1" ]]; then
  if [[ -z "$SPATIAL_SCENE_STEAMVR_ROOT" ]]; then
    SPATIAL_SCENE_STEAMVR_ROOT="$(first_existing_dir \
      "${STEAMVR_ROOT:-}" \
      "${VR_OVERRIDE:-}" \
      "$HOME/.local/share/Steam/steamapps/common/SteamVR" \
      "$HOME/.steam/steam/steamapps/common/SteamVR" \
      "$HOME/.steam/debian-installation/steamapps/common/SteamVR" \
      "$HOME/snap/steam/common/.local/share/Steam/steamapps/common/SteamVR" \
      2>/dev/null || true)"
  fi

  if [[ -z "$SPATIAL_SCENE_STEAM_ROOT" ]]; then
    SPATIAL_SCENE_STEAM_ROOT="$(first_existing_dir \
      "${STEAM_ROOT:-}" \
      "$HOME/.local/share/Steam" \
      "$HOME/.steam/steam" \
      "$HOME/.steam/debian-installation" \
      "$HOME/snap/steam/common/.local/share/Steam" \
      2>/dev/null || true)"
  fi

  APP_LIB_DIR="$(cd "$(dirname "$APP_BIN")/lib" 2>/dev/null && pwd || true)"
  append_ld_path "$APP_LIB_DIR"

  if [[ -n "$SPATIAL_SCENE_STEAMVR_ROOT" ]]; then
    export VR_OVERRIDE="${VR_OVERRIDE:-$SPATIAL_SCENE_STEAMVR_ROOT}"
    append_ld_path "$SPATIAL_SCENE_STEAMVR_ROOT/bin/linux64"
    append_ld_path "$SPATIAL_SCENE_STEAMVR_ROOT/lib/linux64"
    append_ld_path "$SPATIAL_SCENE_STEAMVR_ROOT/bin"
  fi

  if [[ -n "$SPATIAL_SCENE_STEAM_ROOT" ]]; then
    export VR_CONFIG_PATH="${VR_CONFIG_PATH:-$SPATIAL_SCENE_STEAM_ROOT/config}"
    export VR_LOG_PATH="${VR_LOG_PATH:-$SPATIAL_SCENE_STEAM_ROOT/logs}"
    append_ld_path "$SPATIAL_SCENE_STEAM_ROOT/ubuntu12_64"
    append_ld_path "$SPATIAL_SCENE_STEAM_ROOT/ubuntu12_32"
    append_ld_path "$SPATIAL_SCENE_STEAM_ROOT/linux64"
    append_ld_path "$SPATIAL_SCENE_STEAM_ROOT"
  fi
fi

if [[ "$SPATIAL_SCENE_WAIT_STEAMVR" == "1" && "$SPATIAL_SCENE_DRY_RUN" != "1" ]]; then
  deadline=$((SECONDS + SPATIAL_SCENE_STEAMVR_WAIT_TIMEOUT_SEC))
  while (( SECONDS < deadline )); do
    if pgrep -f 'vrserver|vrcompositor' >/dev/null 2>&1; then
      break
    fi
    echo "[start_xr_steamvr_spatial_scene] waiting for SteamVR vrserver/vrcompositor..."
    sleep 1
  done
fi

if [[ ! -x "$APP_BIN" ]]; then
  echo "[start_xr_steamvr_spatial_scene][ERROR] binary not found/executable: $APP_BIN" >&2
  echo "[start_xr_steamvr_spatial_scene][ERROR] Run apps/steamvr/spatial_scene/scripts/linux/install_xr_steamvr_spatial_scene.sh" >&2
  exit 2
fi

mkdir -p "$(dirname "$SPATIAL_SCENE_LOG_FILE")"

echo "== xr_steamvr_spatial_scene =="
echo "ROOT_PROJECT=$ROOT_PROJECT"
echo "APP_BIN=$APP_BIN"
echo "CONFIG=$CONFIG"
echo "registry=$SPATIAL_SCENE_REGISTRY stream=$SPATIAL_SCENE_STREAM"
echo "texture=${SPATIAL_SCENE_WIDTH}x${SPATIAL_SCENE_HEIGHT} render_hz=$SPATIAL_SCENE_RENDER_HZ dry_run=$SPATIAL_SCENE_DRY_RUN"
echo "draw mesh=$SPATIAL_SCENE_DRAW_MESH points=$SPATIAL_SCENE_DRAW_POINTS wire=$SPATIAL_SCENE_DRAW_WIRE alpha=$SPATIAL_SCENE_ALPHA"
echo "log_file=$SPATIAL_SCENE_LOG_FILE"
echo "steamvr_root=${SPATIAL_SCENE_STEAMVR_ROOT:-<not-found>}"
echo "steam_root=${SPATIAL_SCENE_STEAM_ROOT:-<not-found>}"
echo "VR_OVERRIDE=${VR_OVERRIDE:-<unset>}"
echo "VR_CONFIG_PATH=${VR_CONFIG_PATH:-<unset>}"
echo "VR_LOG_PATH=${VR_LOG_PATH:-<unset>}"
echo "LD_LIBRARY_PATH=${LD_LIBRARY_PATH:-<unset>}"
if [[ "$SPATIAL_SCENE_DUMP_OPENVR_LDD" == "1" && "$SPATIAL_SCENE_DRY_RUN" != "1" ]]; then
  pgrep -a -f 'vrserver|vrcompositor|vrmonitor' || true
fi

ARGS=(
  --registry "$SPATIAL_SCENE_REGISTRY"
  --stream "$SPATIAL_SCENE_STREAM"
  --log-file "$SPATIAL_SCENE_LOG_FILE"
  --width "$SPATIAL_SCENE_WIDTH"
  --height "$SPATIAL_SCENE_HEIGHT"
  --render-hz "$SPATIAL_SCENE_RENDER_HZ"
)

if [[ "$SPATIAL_SCENE_DRY_RUN" == "1" ]]; then
  ARGS+=(--dry-run)
fi
if [[ "$SPATIAL_SCENE_VERBOSE_FRAMES" == "1" ]]; then
  ARGS+=(--verbose-frames)
fi

exec "$APP_BIN" "${ARGS[@]}" 2>&1 | tee -a "$SPATIAL_SCENE_LOG_FILE"
