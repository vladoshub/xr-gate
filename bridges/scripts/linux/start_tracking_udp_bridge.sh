#!/usr/bin/env bash
set -euo pipefail

ROOT_PROJECT="${ROOT_PROJECT:-$HOME/src/xr_tracking}"
TRACKING_UDP_BRIDGE_BIN="${TRACKING_UDP_BRIDGE_BIN:-$ROOT_PROJECT/bin/bridges/tracking_udp_bridge}"

REGISTRY="${REGISTRY:-/tmp/tracking_streams.json}"
HMD_STREAM="${HMD_STREAM:-hmd_pose}"
HAND_STREAM="${HAND_STREAM:-hand_tracking}"
TARGET_HOST="${TARGET_HOST:-127.0.0.1}"
TARGET_PORT="${TARGET_PORT:-45670}"
MODE="${MODE:-event}"
SPATIAL_PROXY_MESH_INPUT="${SPATIAL_PROXY_MESH_INPUT:-shm}"
SPATIAL_PROXY_MESH_REGISTRY="${SPATIAL_PROXY_MESH_REGISTRY:-/tmp/runtime_tracking_streams.json}"
SPATIAL_PROXY_MESH_STREAM="${SPATIAL_PROXY_MESH_STREAM:-spatial_proxy_mesh}"
SPATIAL_PROXY_MESH_UDP_HOST="${SPATIAL_PROXY_MESH_UDP_HOST:-$TARGET_HOST}"
SPATIAL_PROXY_MESH_UDP_PORT="${SPATIAL_PROXY_MESH_UDP_PORT:-45740}"
SPATIAL_PROXY_MESH_UDP_MTU_BYTES="${SPATIAL_PROXY_MESH_UDP_MTU_BYTES:-1200}"
SPATIAL_PROXY_MESH_SEND_RATE_HZ="${SPATIAL_PROXY_MESH_SEND_RATE_HZ:-2}"
SPATIAL_PROXY_MESH_DIRECTION="${SPATIAL_PROXY_MESH_DIRECTION:-spatial_backend_to_runtime_adapter}"
SPATIAL_PROXY_MESH_REATTACH_INTERVAL_MS="${SPATIAL_PROXY_MESH_REATTACH_INTERVAL_MS:-500}"

exec "$TRACKING_UDP_BRIDGE_BIN" \
  --registry "$REGISTRY" \
  --hmd-stream "$HMD_STREAM" \
  --hand-stream "$HAND_STREAM" \
  --target-host "$TARGET_HOST" \
  --target-port "$TARGET_PORT" \
  --mode "$MODE" \
  --spatial-proxy-mesh-input "$SPATIAL_PROXY_MESH_INPUT" \
  --spatial-proxy-mesh-registry "$SPATIAL_PROXY_MESH_REGISTRY" \
  --spatial-proxy-mesh-stream "$SPATIAL_PROXY_MESH_STREAM" \
  --spatial-proxy-mesh-udp-host "$SPATIAL_PROXY_MESH_UDP_HOST" \
  --spatial-proxy-mesh-udp-port "$SPATIAL_PROXY_MESH_UDP_PORT" \
  --spatial-proxy-mesh-udp-mtu-bytes "$SPATIAL_PROXY_MESH_UDP_MTU_BYTES" \
  --spatial-proxy-mesh-send-rate-hz "$SPATIAL_PROXY_MESH_SEND_RATE_HZ" \
  --spatial-proxy-mesh-reattach-interval-ms "$SPATIAL_PROXY_MESH_REATTACH_INTERVAL_MS" \
  "$@"
