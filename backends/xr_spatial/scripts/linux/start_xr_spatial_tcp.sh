#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Select capture_service TCP input while preserving all profile, pose, mapper,
# and output behavior from the existing launcher. Automatic TCP profile probing
# remains opt-in, matching the rest of the backends.
export CAPTURE_TRANSPORT="${CAPTURE_TRANSPORT:-capture_tcp}"
export CAPTURE_PROFILE_PROBE_ENABLED="${CAPTURE_PROFILE_PROBE_ENABLED:-0}"
exec "$SCRIPT_DIR/start_xr_spatial_shm.sh" "$@"
