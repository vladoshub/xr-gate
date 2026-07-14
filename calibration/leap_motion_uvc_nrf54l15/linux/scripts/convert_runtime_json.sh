#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CALIBRATION_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
export CALIB_TARGET=leap_motion_uvc_nrf54l15
exec "$CALIBRATION_ROOT/common/linux/calibrate.sh" --target "$CALIB_TARGET" convert-runtime "$@"
