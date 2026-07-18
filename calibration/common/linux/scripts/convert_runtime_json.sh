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
echo "IMU_UPDATE_RATE=$IMU_UPDATE_RATE"
echo "IMU_ACCEL_NOISE_DENSITY=$IMU_ACCEL_NOISE_DENSITY"
echo "IMU_ACCEL_RANDOM_WALK=$IMU_ACCEL_RANDOM_WALK"
echo "IMU_GYRO_NOISE_DENSITY=$IMU_GYRO_NOISE_DENSITY"
echo "IMU_GYRO_RANDOM_WALK=$IMU_GYRO_RANDOM_WALK"

python3 "$CONVERTER_TO_RUNTIME" \
  --camchain "$CAMCHAIN" \
  --out "$BASALT_OUT" \
  --imu-update-rate "$IMU_UPDATE_RATE" \
  --accel-noise-density "$IMU_ACCEL_NOISE_DENSITY" \
  --accel-random-walk "$IMU_ACCEL_RANDOM_WALK" \
  --gyro-noise-density "$IMU_GYRO_NOISE_DENSITY" \
  --gyro-random-walk "$IMU_GYRO_RANDOM_WALK"

python3 - "$BASALT_OUT" "$IMU_UPDATE_RATE" <<'PY'
import json
import math
import sys
from pathlib import Path

path = Path(sys.argv[1])
expected_rate = float(sys.argv[2])
data = json.loads(path.read_text(encoding="utf-8"))
if not isinstance(data, dict) or not isinstance(data.get("value0"), dict):
    raise SystemExit(f"[convert-runtime][ERROR] invalid JSON root in {path}")
value = data["value0"]
if len(value.get("resolution", [])) != 2:
    raise SystemExit("[convert-runtime][ERROR] expected two camera resolutions")
if len(value.get("T_imu_cam", [])) != 2:
    raise SystemExit("[convert-runtime][ERROR] expected two camera-to-IMU poses")
if len(value.get("intrinsics", [])) != 2:
    raise SystemExit("[convert-runtime][ERROR] expected two camera intrinsics blocks")
actual_rate = float(value.get("imu_update_rate", float("nan")))
if not math.isclose(actual_rate, expected_rate, rel_tol=0.0, abs_tol=1e-9):
    raise SystemExit(
        f"[convert-runtime][ERROR] IMU rate mismatch: expected {expected_rate}, got {actual_rate}"
    )
if not all(cam.get("camera_type") == "kb4" for cam in value["intrinsics"]):
    raise SystemExit("[convert-runtime][ERROR] expected kb4 runtime cameras")
print(f"[convert-runtime] validated {path}")
PY

cp "$BASALT_OUT" "$MERCURY_OUT"

echo "[OK] runtime calibration JSON files created"
ls -lh "$BASALT_OUT" "$MERCURY_OUT"
if command -v jq >/dev/null 2>&1; then
  jq '.value0.resolution,
      .value0.imu_update_rate,
      .value0.cam_time_offset_ns,
      .value0.T_imu_cam' "$BASALT_OUT"
fi
