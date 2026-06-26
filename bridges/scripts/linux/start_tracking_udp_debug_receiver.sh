#!/usr/bin/env bash
set -euo pipefail

ROOT_PROJECT="${ROOT_PROJECT:-$HOME/src/xr_tracking}"
TRACKING_UDP_DEBUG_RECEIVER_BIN="${TRACKING_UDP_DEBUG_RECEIVER_BIN:-$ROOT_PROJECT/bin/bridges/tracking_udp_debug_receiver}"

LISTEN_HOST="${LISTEN_HOST:-0.0.0.0}"
LISTEN_PORT="${LISTEN_PORT:-45670}"

exec "$TRACKING_UDP_DEBUG_RECEIVER_BIN" \
  --bind-host "$LISTEN_HOST" \
  --bind-port "$LISTEN_PORT" \
  "$@"
