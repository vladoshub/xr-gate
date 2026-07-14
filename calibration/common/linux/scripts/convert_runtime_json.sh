#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=load_target.sh
source "$SCRIPT_DIR/load_target.sh"

CAMCHAIN="$(expand_tilde "${CAMCHAIN:-$FINAL_PROFILE_DIR/camchain-imucam.yaml}")"
BASALT_OUT="$(expand_tilde "${BASALT_OUT:-$FINAL_PROFILE_DIR/basalt_calib_${CALIB_PROFILE_NAME}.json}")"
MERCURY_OUT="$(expand_tilde "${MERCURY_OUT:-$FINAL_PROFILE_DIR/mercury_calib_${CALIB_PROFILE_NAME}.json}")"

[[ -f "$CONVERTER_TO_RUNTIME" ]] || { echo "[convert-runtime][ERROR] converter missing: $CONVERTER_TO_RUNTIME" >&2; exit 1; }
[[ -f "$CAMCHAIN" ]] || { echo "[convert-runtime][ERROR] camchain missing: $CAMCHAIN" >&2; exit 1; }
mkdir -p "$FINAL_PROFILE_DIR"

print_target_summary
echo "CAMCHAIN=$CAMCHAIN"
echo "BASALT_OUT=$BASALT_OUT"
echo "MERCURY_OUT=$MERCURY_OUT"

python3 "$CONVERTER_TO_RUNTIME" --camchain "$CAMCHAIN" --out "$BASALT_OUT"
cp "$BASALT_OUT" "$MERCURY_OUT"

echo "[OK] runtime calibration JSON files created"
ls -lh "$BASALT_OUT" "$MERCURY_OUT"
if command -v jq >/dev/null 2>&1; then
  jq '.value0.resolution, .value0.cam_time_offset_ns, .value0.T_imu_cam' "$BASALT_OUT"
fi
