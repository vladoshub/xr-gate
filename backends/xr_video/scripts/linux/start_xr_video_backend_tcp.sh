#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export INPUT_TRANSPORT="${INPUT_TRANSPORT:-capture_tcp}"
export CAPTURE_PROFILE_PROBE_ENABLED="${CAPTURE_PROFILE_PROBE_ENABLED:-0}"
exec "$SCRIPT_DIR/start_xr_video_backend.sh" "$@"
