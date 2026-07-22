#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=load_target.sh
source "$SCRIPT_DIR/load_target.sh"

usage() {
  cat <<USAGE
Usage: ${0##*/} [--no-imu]

Options:
  --no-imu  Convert the saved stereo camera-only camchain. The converter uses
            cam0 as a synthetic body frame and does not require T_cam_imu.
  -h, --help
            Show this help.

Environment overrides such as CAMCHAIN, BASALT_OUT, MERCURY_OUT,
BASALT_VIO_OUT, BASALT_VO_OUT, BASALT_VO_CONFIG_TEMPLATE and
BASALT_VO_CONFIG_OVERRIDES remain supported.
USAGE
}

NO_IMU="${NO_IMU:-0}"
while [[ $# -gt 0 ]]; do
  case "$1" in
    --no-imu)
      NO_IMU=1
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "[convert-runtime][ERROR] unknown argument: $1" >&2
      usage >&2
      exit 2
      ;;
  esac
done

case "$NO_IMU" in
  0|1) ;;
  *) echo "[convert-runtime][ERROR] NO_IMU must be 0 or 1, got: $NO_IMU" >&2; exit 2 ;;
esac

if [[ -n "${CAMCHAIN:-}" ]]; then
  CAMCHAIN="$(expand_tilde "$CAMCHAIN")"
elif [[ "$NO_IMU" == "1" ]]; then
  CAMCHAIN="$CAMERA_PROFILE_DIR/$CAMCHAIN_OUTPUT_NAME"
else
  CAMCHAIN="$FINAL_PROFILE_DIR/camchain-imucam.yaml"
fi

BASALT_OUT="$(expand_tilde "${BASALT_OUT:-$FINAL_PROFILE_DIR/basalt_calib_${CALIB_PROFILE_NAME}.json}")"
MERCURY_OUT="$(expand_tilde "${MERCURY_OUT:-$FINAL_PROFILE_DIR/mercury_calib_${CALIB_PROFILE_NAME}.json}")"
BASALT_VIO_OUT="$(expand_tilde "${BASALT_VIO_OUT:-$FINAL_PROFILE_DIR/$BASALT_VIO_CONFIG_NAME}")"
FORCE_BASALT_VIO_CONFIG="${FORCE_BASALT_VIO_CONFIG:-0}"

BASALT_VO_CONFIG_NAME="${BASALT_VO_CONFIG_NAME:-basalt_vo_config_${CALIB_PROFILE_NAME}.json}"
BASALT_VO_OUT="$(expand_tilde "${BASALT_VO_OUT:-$FINAL_PROFILE_DIR/$BASALT_VO_CONFIG_NAME}")"
BASALT_VO_CONFIG_TEMPLATE_DEFAULT="${XR_CALIB_COMMON_DIR:-$SCRIPT_DIR}/templates/euroc_config_vo.json"
BASALT_VO_CONFIG_TEMPLATE="$(expand_tilde "${BASALT_VO_CONFIG_TEMPLATE:-$BASALT_VO_CONFIG_TEMPLATE_DEFAULT}")"
BASALT_VO_CONFIG_OVERRIDES="${BASALT_VO_CONFIG_OVERRIDES:-}"
if [[ -n "$BASALT_VO_CONFIG_OVERRIDES" ]]; then
  BASALT_VO_CONFIG_OVERRIDES="$(expand_tilde "$BASALT_VO_CONFIG_OVERRIDES")"
fi
FORCE_BASALT_VO_CONFIG="${FORCE_BASALT_VO_CONFIG:-0}"

[[ -f "$CONVERTER_TO_RUNTIME" ]] || { echo "[convert-runtime][ERROR] converter missing: $CONVERTER_TO_RUNTIME" >&2; exit 1; }
[[ -f "$CAMCHAIN" ]] || { echo "[convert-runtime][ERROR] camchain missing: $CAMCHAIN" >&2; exit 1; }
[[ -f "$BASALT_VIO_CONFIG_TEMPLATE" ]] || { echo "[convert-runtime][ERROR] Basalt VIO config template missing: $BASALT_VIO_CONFIG_TEMPLATE" >&2; exit 1; }
[[ -f "$BASALT_VO_CONFIG_TEMPLATE" ]] || { echo "[convert-runtime][ERROR] Basalt VO config template missing: $BASALT_VO_CONFIG_TEMPLATE" >&2; exit 1; }
if [[ -n "$BASALT_VO_CONFIG_OVERRIDES" && ! -f "$BASALT_VO_CONFIG_OVERRIDES" ]]; then
  echo "[convert-runtime][ERROR] Basalt VO config overrides missing: $BASALT_VO_CONFIG_OVERRIDES" >&2
  exit 1
fi
mkdir -p "$FINAL_PROFILE_DIR"

MODE="stereo_vio"
if [[ "$NO_IMU" == "1" ]]; then
  MODE="stereo_vo_no_imu"
fi

print_target_summary
echo "MODE=$MODE"
echo "CAMCHAIN=$CAMCHAIN"
echo "BASALT_OUT=$BASALT_OUT"
echo "MERCURY_OUT=$MERCURY_OUT"
echo "BASALT_VIO_CONFIG_TEMPLATE=$BASALT_VIO_CONFIG_TEMPLATE"
echo "BASALT_VIO_OUT=$BASALT_VIO_OUT"
echo "BASALT_VO_CONFIG_TEMPLATE=$BASALT_VO_CONFIG_TEMPLATE"
echo "BASALT_VO_CONFIG_OVERRIDES=${BASALT_VO_CONFIG_OVERRIDES:-<none>}"
echo "BASALT_VO_OUT=$BASALT_VO_OUT"
echo "IMU_UPDATE_RATE=$IMU_UPDATE_RATE"
echo "IMU_ACCEL_NOISE_DENSITY=$IMU_ACCEL_NOISE_DENSITY"
echo "IMU_ACCEL_RANDOM_WALK=$IMU_ACCEL_RANDOM_WALK"
echo "IMU_GYRO_NOISE_DENSITY=$IMU_GYRO_NOISE_DENSITY"
echo "IMU_GYRO_RANDOM_WALK=$IMU_GYRO_RANDOM_WALK"
if [[ "$NO_IMU" == "1" ]]; then
  echo "[convert-runtime] no-IMU mode: T_imu_cam[0] is identity, T_imu_cam[1] is derived from cam1.T_cn_cnm1"
  echo "[convert-runtime] no-IMU mode: IMU rate/noise fields are schema placeholders and are ignored by Basalt --no-imu"
fi

converter_args=(
  --camchain "$CAMCHAIN"
  --out "$BASALT_OUT"
  --imu-update-rate "$IMU_UPDATE_RATE"
  --accel-noise-density "$IMU_ACCEL_NOISE_DENSITY"
  --accel-random-walk "$IMU_ACCEL_RANDOM_WALK"
  --gyro-noise-density "$IMU_GYRO_NOISE_DENSITY"
  --gyro-random-walk "$IMU_GYRO_RANDOM_WALK"
)
if [[ "$NO_IMU" == "1" ]]; then
  converter_args+=(--no-imu)
fi
python3 "$CONVERTER_TO_RUNTIME" "${converter_args[@]}"

python3 - "$BASALT_OUT" "$IMU_UPDATE_RATE" "$NO_IMU" <<'PY'
import json
import math
import sys
from pathlib import Path

path = Path(sys.argv[1])
expected_rate = float(sys.argv[2])
no_imu = sys.argv[3] == "1"
data = json.loads(path.read_text(encoding="utf-8"))
if not isinstance(data, dict) or not isinstance(data.get("value0"), dict):
    raise SystemExit(f"[convert-runtime][ERROR] invalid JSON root in {path}")
value = data["value0"]
if len(value.get("resolution", [])) != 2:
    raise SystemExit("[convert-runtime][ERROR] expected two camera resolutions")
poses = value.get("T_imu_cam", [])
if len(poses) != 2:
    raise SystemExit("[convert-runtime][ERROR] expected two camera-to-body poses")
if len(value.get("intrinsics", [])) != 2:
    raise SystemExit("[convert-runtime][ERROR] expected two camera intrinsics blocks")
actual_rate = float(value.get("imu_update_rate", float("nan")))
if not math.isclose(actual_rate, expected_rate, rel_tol=0.0, abs_tol=1e-9):
    raise SystemExit(
        f"[convert-runtime][ERROR] IMU rate mismatch: expected {expected_rate}, got {actual_rate}"
    )
if not all(cam.get("camera_type") == "kb4" for cam in value["intrinsics"]):
    raise SystemExit("[convert-runtime][ERROR] expected kb4 runtime cameras")
if no_imu:
    identity = {
        "px": 0.0,
        "py": 0.0,
        "pz": 0.0,
        "qx": 0.0,
        "qy": 0.0,
        "qz": 0.0,
        "qw": 1.0,
    }
    for key, expected in identity.items():
        actual = float(poses[0].get(key, float("nan")))
        if not math.isclose(actual, expected, rel_tol=0.0, abs_tol=1e-9):
            raise SystemExit(
                f"[convert-runtime][ERROR] no-IMU cam0 body pose is not identity: {key}={actual}"
            )
    if int(value.get("cam_time_offset_ns", -1)) != 0:
        raise SystemExit("[convert-runtime][ERROR] no-IMU cam_time_offset_ns must be zero")
print(f"[convert-runtime] validated {path}")
PY

cp "$BASALT_OUT" "$MERCURY_OUT"

if [[ ! -e "$BASALT_VIO_OUT" || "$FORCE_BASALT_VIO_CONFIG" == "1" ]]; then
  install -Dm0644 "$BASALT_VIO_CONFIG_TEMPLATE" "$BASALT_VIO_OUT"
  echo "[convert-runtime] installed Basalt VIO config: $BASALT_VIO_OUT"
else
  echo "[convert-runtime] preserving existing Basalt VIO config: $BASALT_VIO_OUT"
  echo "[convert-runtime] set FORCE_BASALT_VIO_CONFIG=1 to replace it from the template"
fi

python3 - "$BASALT_VIO_OUT" <<'PY_VIO'
import json
import sys
from pathlib import Path

path = Path(sys.argv[1])
try:
    data = json.loads(path.read_text(encoding="utf-8"))
except (OSError, json.JSONDecodeError) as exc:
    raise SystemExit(f"[convert-runtime][ERROR] invalid Basalt VIO config {path}: {exc}")
if not isinstance(data, dict) or not isinstance(data.get("value0"), dict):
    raise SystemExit(
        f"[convert-runtime][ERROR] Basalt VIO config must contain an object at value0: {path}"
    )
if not data["value0"]:
    raise SystemExit(f"[convert-runtime][ERROR] Basalt VIO config value0 is empty: {path}")
print(f"[convert-runtime] validated {path}")
PY_VIO

# Build a dedicated stereo-VO algorithm config from the upstream EuRoC VO
# baseline plus an optional device-specific JSON overlay.
if [[ ! -e "$BASALT_VO_OUT" || "$FORCE_BASALT_VO_CONFIG" == "1" ]]; then
  BASALT_VO_RENDERED="$(mktemp)"
  trap 'rm -f "${BASALT_VO_RENDERED:-}"' EXIT

  python3 - \
    "$BASALT_VO_CONFIG_TEMPLATE" \
    "$BASALT_VO_CONFIG_OVERRIDES" \
    "$BASALT_VO_RENDERED" <<'PY_VO'
import json
import sys
from pathlib import Path

template_path = Path(sys.argv[1])
overrides_arg = sys.argv[2]
out_path = Path(sys.argv[3])

try:
    data = json.loads(template_path.read_text(encoding="utf-8"))
except (OSError, json.JSONDecodeError) as exc:
    raise SystemExit(
        f"[convert-runtime][ERROR] invalid Basalt VO template {template_path}: {exc}"
    )

if not isinstance(data, dict) or not isinstance(data.get("value0"), dict):
    raise SystemExit(
        f"[convert-runtime][ERROR] Basalt VO template must contain an object at value0: "
        f"{template_path}"
    )

if overrides_arg:
    overrides_path = Path(overrides_arg)
    try:
        overrides_doc = json.loads(overrides_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise SystemExit(
            f"[convert-runtime][ERROR] invalid Basalt VO overrides "
            f"{overrides_path}: {exc}"
        )

    # Accept either {"value0": {...}} or a direct {"config.*": ...} object.
    overrides = overrides_doc.get("value0", overrides_doc)
    if not isinstance(overrides, dict):
        raise SystemExit(
            f"[convert-runtime][ERROR] Basalt VO overrides must be an object: "
            f"{overrides_path}"
        )
    data["value0"].update(overrides)

out_path.write_text(
    json.dumps(data, indent=4, ensure_ascii=False) + "\n",
    encoding="utf-8",
)
PY_VO

  install -Dm0644 "$BASALT_VO_RENDERED" "$BASALT_VO_OUT"
  echo "[convert-runtime] installed Basalt VO config: $BASALT_VO_OUT"
else
  echo "[convert-runtime] preserving existing Basalt VO config: $BASALT_VO_OUT"
  echo "[convert-runtime] set FORCE_BASALT_VO_CONFIG=1 to replace it from template/overrides"
fi

python3 - "$BASALT_VO_OUT" <<'PY_VO_VALIDATE'
import json
import sys
from pathlib import Path

path = Path(sys.argv[1])
try:
    data = json.loads(path.read_text(encoding="utf-8"))
except (OSError, json.JSONDecodeError) as exc:
    raise SystemExit(f"[convert-runtime][ERROR] invalid Basalt VO config {path}: {exc}")

if not isinstance(data, dict) or not isinstance(data.get("value0"), dict):
    raise SystemExit(
        f"[convert-runtime][ERROR] Basalt VO config must contain an object at value0: "
        f"{path}"
    )

value = data["value0"]
required = {
    "config.optical_flow_type",
    "config.optical_flow_epipolar_error",
    "config.vio_linearization_type",
    "config.vio_scale_jacobian",
}
missing = sorted(required - value.keys())
if missing:
    raise SystemExit(
        f"[convert-runtime][ERROR] Basalt VO config is missing required keys: "
        f"{', '.join(missing)}"
    )

print(f"[convert-runtime] validated {path}")
PY_VO_VALIDATE

echo "[OK] runtime calibration, Basalt VIO and Basalt VO JSON files created"
ls -lh "$BASALT_OUT" "$MERCURY_OUT" "$BASALT_VIO_OUT" "$BASALT_VO_OUT"
if command -v jq >/dev/null 2>&1; then
  jq '.value0.resolution,
      .value0.imu_update_rate,
      .value0.cam_time_offset_ns,
      .value0.T_imu_cam' "$BASALT_OUT"
fi
