#!/usr/bin/env bash
set -euo pipefail

ROOT_PROJECT="${ROOT_PROJECT:-$HOME/src/xr_tracking}"
CAPTURE_NET_BRIDGE_BIN="${CAPTURE_NET_BRIDGE_BIN:-$ROOT_PROJECT/bin/bridges/capture_net_bridge}"

REGISTRY="${REGISTRY:-/tmp/capture_service_streams.json}"
LISTEN_HOST="${LISTEN_HOST:-127.0.0.1}"
LISTEN_PORT="${LISTEN_PORT:-45555}"
CAM0_STREAM="${CAM0_STREAM:-camera0}"
CAM1_STREAM="${CAM1_STREAM:-camera1}"
IMU_STREAM="${IMU_STREAM:-imu0}"

exec "$CAPTURE_NET_BRIDGE_BIN" \
  --registry "$REGISTRY" \
  --listen-host "$LISTEN_HOST" \
  --listen-port "$LISTEN_PORT" \
  --cam0-stream "$CAM0_STREAM" \
  --cam1-stream "$CAM1_STREAM" \
  --imu-stream "$IMU_STREAM" \
  "$@"
