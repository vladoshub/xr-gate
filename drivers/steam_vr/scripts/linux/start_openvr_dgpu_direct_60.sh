#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export XR_OPENVR_DGPU_FREQ_HZ="${XR_OPENVR_DGPU_FREQ_HZ:-60}"
exec "$SCRIPT_DIR/start_openvr_dgpu_direct.sh" "$@"
