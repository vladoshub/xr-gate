# Stereo Camera + IMU Calibration — Actions

This guide assumes that the common calibration patch has already been applied.

Required streams:

```text
camera0 — left GRAY8 image
camera1 — right GRAY8 image
imu0    — gyro in rad/s, accelerometer in m/s² (Optional)
```

The stereo camera and IMU must be rigidly mounted.


Build 

```bash
./devices/common/linux/scripts/build/install_xr_gate_out.sh
```


Prepare device_profile (see others in ~/xr-gate/capture_service_cpp/configs/)

Start `capture_service_cpp`.

capture_service_cpp bin unpacked in ~/xr-gate/out

```bash
CONFIG_PATH=~/xr-gate/devices/<device>/configs/capture_service/<device>.yaml ~/xr-gate/out/xr-gate/bin/scripts/capture_service_cpp/start_capture_service_cpp.sh
```

Example:

```bash
CONFIG_PATH=~/xr-gate/devices/leap_motion_uvc_nrf54l15/configs/capture_service/leap_motion_uvc_nrf54l15.yaml ~/xr-gate/out/xr-gate/bin/scripts/capture_service_cpp/start_capture_service_cpp.sh
```

## 1. Calibrate the IMU mount axes (Skip if no IMU)


Temporarily remove any existing:

```yaml
imu:
  transform:
```

Open new terminal

Find the calibration tool:

```bash
ROOT="$HOME/xr-gate"
AXIS_TOOL="$(find "$ROOT" -type f \
  -path '*/capture_service_cpp/tools/calibrate_imu_axes.py' \
  | head -n1)"
```

Run:

```bash
python3 "$AXIS_TOOL" \
  --registry /tmp/capture_service_streams.json \
  --stream imu0 \
  --output "/tmp/${CALIB_TARGET}_${CALIB_UNIT_ID}_imu_mount.json"
```

Perform the guided movements:

```text
still
roll
pitch
yaw
still
```

Save IMU rate

Example:

```bash
estimated_sample_rate_hz=208.0
```

Apply the generated result:

```yaml
imu:
  transform:
    axes: [x, -z, y]
```

or:

```yaml
imu:
  transform:
    quaternion_xyzw: [qx, qy, qz, qw]
```

Restart capture and run the tool again.

The verification result should be close to identity. Do not apply the verification result as another transform.

Keep this transform unchanged during recording, Kalibr calibration, and runtime operation.


---

## 2. Create a target profile

Create:

```text
calibration/common/linux/profiles/<target>.env
```

target = name = CALIB_TARGET_NAME
Example:
```text
calibration/common/linux/profiles/my_stereo_imu.env
```

```bash
CALIB_TARGET_NAME="my_stereo_imu"
CALIB_LABEL="My Stereo Camera + IMU"
CALIB_DEVICE_NAME="my_stereo_imu"

CALIB_UNIT_ID="${CALIB_UNIT_ID:-mount_v1_unit_001}"
CALIB_PROFILE_NAME="${CALIB_PROFILE_NAME:-stereo_640x480_none}"

#GET FROM config in capture_service_cpp
EXPECT_WIDTH=640
EXPECT_HEIGHT=480
RECORD_PREFIX="my_stereo_imu_640x480_calib"

#Example:
#CAPTURE_CONFIG_CAMERA_ONLY="${CAPTURE_CONFIG_CAMERA_ONLY:-$ROOT_PROJECT/devices/leap_motion_uvc_nrf54l15/configs/capture_service/leap_motion_uvc_nrf54l15.yaml}"

CAPTURE_CONFIG_CAMERA_ONLY="${CAPTURE_CONFIG_CAMERA_ONLY:-$ROOT_PROJECT/devices/my_device/configs/capture_service/my_camera_only.yaml}"

#Exmaple:
#CAPTURE_CONFIG_STEREO_IMU="${CAPTURE_CONFIG_STEREO_IMU:-$ROOT_PROJECT/devices/leap_motion_uvc_nrf54l15/configs/capture_service/leap_motion_uvc_nrf54l15.yaml}"

CAPTURE_CONFIG_STEREO_IMU="${CAPTURE_CONFIG_STEREO_IMU:-$ROOT_PROJECT/devices/my_device/configs/capture_service/my_stereo_imu.yaml}"

CAPTURE_CONFIG="$CAPTURE_CONFIG_STEREO_IMU"

CAMERA_MODEL_0="pinhole-equi"
CAMERA_MODEL_1="pinhole-equi"

IMU_YAML_NAME="imu_my_sensor.yaml"

# Replace with the measured published imu0 rate estimated_sample_rate_hz from 1 step!
#If no IMU just no change
IMU_UPDATE_RATE=208.0

# Initial values. Replace with measured values when available.
IMU_ACCEL_NOISE_DENSITY=0.01
IMU_ACCEL_RANDOM_WALK=0.001
IMU_GYRO_NOISE_DENSITY=0.001
IMU_GYRO_RANDOM_WALK=0.0001

DEFAULT_RECORD_MODE="stereo_imu"
```

No IMU
```bash
CALIB_TARGET_NAME="my_stereo_camera"
CALIB_LABEL="My Stereo Camera"
CALIB_DEVICE_NAME="my_stereo_camera"

CALIB_UNIT_ID="${CALIB_UNIT_ID:-camera_unit_001}"
CALIB_PROFILE_NAME="${CALIB_PROFILE_NAME:-stereo_640x480_none}"

EXPECT_WIDTH=640
EXPECT_HEIGHT=480
RECORD_PREFIX="my_stereo_camera_640x480_calib"

CAPTURE_CONFIG_CAMERA_ONLY="${CAPTURE_CONFIG_CAMERA_ONLY:-$ROOT_PROJECT/devices/my_device/configs/capture_service/my_camera_only.yaml}"
CAPTURE_CONFIG="$CAPTURE_CONFIG_CAMERA_ONLY"

CAMERA_MODEL_0="pinhole-equi"
CAMERA_MODEL_1="pinhole-equi"

# The common target schema may still generate a technical IMU YAML.
IMU_YAML_NAME="imu_unused.yaml"

# Placeholders required by the current Basalt calibration JSON schema.
# Basalt started with --no-imu does not use these values.
IMU_UPDATE_RATE=200.0
IMU_ACCEL_NOISE_DENSITY=0.01
IMU_ACCEL_RANDOM_WALK=0.001
IMU_GYRO_NOISE_DENSITY=0.001
IMU_GYRO_RANDOM_WALK=0.0001

DEFAULT_RECORD_MODE="camera_only"

```

Use a unique `CALIB_UNIT_ID` for every physical assembly or mount revision.

---

## 2. Create the capture-service config

The final stereo+IMU config must publish:

```text
camera0
camera1
imu0 (optional)
```

Requirements:

```text
camera0 is always left
camera1 is always right
both images have the same final resolution
images are GRAY8
gyro is rad/s
accelerometer is m/s²
timestamps are monotonic
```

Keep these identical during calibration and runtime:

```text
resolution
crop
rotation and flip
left/right order
IMU axis transform
camera timestamp mode
IMU timestamp mode
IMU sample rate
```

For serial IMUs, use a stable path:

```yaml
port:
  linux: /dev/serial/by-id/<stable-device-id>
```

---

## 3. Install Kalibr and generate target files

```bash
ROOT="$HOME/xr-gate"
CAL_COMMON="$ROOT/calibration/common/linux"

export CALIB_TARGET="my_stereo_imu"
export CALIB_UNIT_ID="mount_v1_unit_001"
export CALIB_PROFILE_NAME="stereo_640x480_none"
```

No IMU

```bash
ROOT="$HOME/xr-gate"
CAL_COMMON="$ROOT/calibration/common/linux"

export CALIB_TARGET="my_stereo_camera"
export CALIB_UNIT_ID="mount_v1_unit_001"
export CALIB_PROFILE_NAME="stereo_640x480_none"
```

Check the resolved profile:

```bash
"$CAL_COMMON/calibrate.sh" \
  --target "$CALIB_TARGET" \
  show-target
```

Install:

```bash
FORCE_TARGET_CONFIGS=1 \
"$CAL_COMMON/calibrate.sh" \
  --target "$CALIB_TARGET" \
  install
```

Check the generated IMU YAML (Skip if no IMU):

```bash
cat "$HOME/xr_calib/$CALIB_TARGET"/imu*.yaml
```

It must contain:

```yaml
rostopic: /imu0
update_rate: <actual published rate>
```

---

## 5. Set the real IMU rate

Set your real IMU rate (Skip if no IMU):

```bash
IMU_UPDATE_RATE=208.0
```

Regenerate the IMU YAML:

```bash
FORCE_TARGET_CONFIGS=1 \
"$CAL_COMMON/calibrate.sh" \
  --target "$CALIB_TARGET" \
  install
```

Check the YAML again before calibration.

---

## 6. Prepare the AprilGrid

```bash
cat "$HOME/xr_calib/$CALIB_TARGET/targets/"*.yaml
```

Print at 100% scale with “fit to page” disabled.

Mount the print on a rigid flat surface.

Verify that the physical tag size matches:

```bash
ROW_MM=171.1
COL_MM=243.7

python3 - <<PY
row_mm = float("$ROW_MM")
col_mm = float("$COL_MM")

tag_from_row = row_mm / 6.6
tag_from_col = col_mm / 9.4
avg = (tag_from_row + tag_from_col) / 2

print("tag from row mm:", tag_from_row)
print("tag from col mm:", tag_from_col)
print("avg tag mm:", avg)
print("tagSize meters:", avg / 1000.0)
PY
```

Then update `tagSize` in `aprilgrid_a4_5x7_25mm.yaml`.


```yaml
tagSize: <metres>
tagSpacing: <gap divided by tag size>
```

---

## 7. Record one stereo+IMU dataset

Use one recording for both stereo calibration and camera–IMU calibration:

Before starting, you must stop all running capture_service_cpp


You can prevent run capture_service_cpp:

```bash
CONFIG_PATH=~/xr-gate/devices/leap_motion_uvc/configs/capture_service/leap_motion_uvc.yaml ~/xr-gate-release/xr-gate/bin/scripts/capture_service_cpp/start_capture_service_cpp.sh
```

If you prevent run capture_service_cpp start calibrate.sh START_CAPTURE_SERVICE=0 

```bash
RECORD_MODE=stereo_imu \
START_CAPTURE_SERVICE=1 \
SECONDS_TOTAL=90 \
"$CAL_COMMON/calibrate.sh" \
  --target "$CALIB_TARGET" \
  record
```

If no IMU:

```bash
RECORD_MODE=camera_only \
START_CAPTURE_SERVICE=0 \
SECONDS_TOTAL=90 \
"$CAL_COMMON/calibrate.sh" \
  --target "$CALIB_TARGET" \
  record
```

After start and press enter run parallel in new terminal for check camera:

```bash
CAPTURE_CLIENT_ROOT=~/xr-gate/capture_client ./xr-gate/capture_client/debug/direct_slam_viewer_shm.sh
```

Keep the AprilGrid still and move the entire camera+IMU assembly.

Perform:

```text
still
yaw
pitch
roll
X/Y translation
closer/farther translation
combined smooth movement
still
```

Show the grid at different distances, angles, and image positions.

Keep it visible in both cameras and avoid motion blur.

---

## 8. Verify the dataset and create the ROS bag

Select the latest dataset:

```bash
DS="$(ls -1dt "$HOME/xr_records/$CALIB_TARGET"/* | head -n1)"
echo "$DS"
```

Check:

```bash
wc -l "$DS/camera_timestamps.csv"
wc -l "$DS/imu.csv"
head -3 "$DS/imu.csv"
cat "$DS/record_metadata.json"
```

Create the bag:

```bash
BAG="$HOME/xr_calib/$CALIB_TARGET/bags/$(basename "$DS")_stereo_imu.bag"

DS="$DS" \
BAG="$BAG" \
NO_IMU=0 \
"$CAL_COMMON/calibrate.sh" \
  --target "$CALIB_TARGET" \
  bag
```

If no IMU:

```bash
BAG="$HOME/xr_calib/$CALIB_TARGET/bags/$(basename "$DS")_stereo_imu.bag"

DS="$DS" \
BAG="$BAG" \
NO_IMU=1 \
"$CAL_COMMON/calibrate.sh" \
  --target "$CALIB_TARGET" \
  bag
```



For stereo+IMU mode, the bag must contain:

```text
/cam0/image_raw
/cam1/image_raw
/imu0
```

For camera-only mode, the bag must contain:

```text
/cam0/image_raw
/cam1/image_raw
```

---

## 9. Run stereo camera calibration

```bash
BAG="$BAG" \
BAG_FREQ=4.0 \
APPROX_SYNC=0.005 \
"$CAL_COMMON/calibrate.sh" \
  --target "$CALIB_TARGET" \
  camera
```

Save the result:

```bash
BAG="$BAG" \
"$CAL_COMMON/calibrate.sh" \
  --target "$CALIB_TARGET" \
  save-camera
```

Check:

```text
finite intrinsics
physically plausible stereo baseline
physically plausible camera rotation
low reprojection error
no strong systematic residual pattern in the report
```

A practical target is below approximately `0.5 px`.

---

## 10. Run camera–IMU calibration (Skip if no IMU)

For a 90-second bag:

```bash
BAG="$BAG" \
FROM=3 \
TO=87 \
MAX_ITER=7 \
TIMEOFFSET_PADDING=0.5 \
"$CAL_COMMON/calibrate.sh" \
  --target "$CALIB_TARGET" \
  imu-camera
```

Accept the result when:

```text
the optimizer converges
cam0 and cam1 time offsets are nearly equal
gravity magnitude is near 9.81 m/s²
camera–IMU translation matches the physical mount
camera–IMU rotation matches the physical mount
```

Use `MAX_ITER=20` only when the final iterations are still improving significantly.

---

## 11. Convert to runtime JSON


###chmod +x calibration/common/linux/scripts/convert_runtime_json.sh

```bash
FORCE_BASALT_VO_CONFIG=1 \
"$CAL_COMMON/calibrate.sh" \
  --target "$CALIB_TARGET" \
  convert-runtime
```

If no IMU:

```bash
FORCE_BASALT_VO_CONFIG=1 \
"$CAL_COMMON/calibrate.sh" \
  --target "$CALIB_TARGET" \
  convert-runtime --no-imu
```

The converter uses these target-profile values:

```text
IMU_UPDATE_RATE
IMU_ACCEL_NOISE_DENSITY
IMU_ACCEL_RANDOM_WALK
IMU_GYRO_NOISE_DENSITY
IMU_GYRO_RANDOM_WALK
```

The supported camera conversion is:

```text
Kalibr pinhole-equi → runtime kb4
```

The command creates three runtime files in the final profile directory:

```text
basalt_calib_<profile>.json
mercury_calib_<profile>.json
basalt_vio_config_<target-specific-name>.json
basalt_vo_config_<target-specific-name>.json
```

The VIO file is copied from the shared algorithm template; it is not derived
from Kalibr camera/IMU measurements. Existing VIO tuning is preserved. Use
`FORCE_BASALT_VIO_CONFIG=1` only when the file should be reset to the template.

Verify:

```bash
FINAL="$HOME/xr_calib/$CALIB_TARGET/final"

jq '
  .value0.resolution,
  .value0.imu_update_rate,
  .value0.cam_time_offset_ns,
  .value0.T_imu_cam
' "$FINAL"/*/*/*/basalt_calib_*.json

jq -e '.value0 | type == "object" and length > 0' \
  "$FINAL"/*/*/*/basalt_vio_config_*.json
```

---

## 12. Install the runtime calibration

Create a runtime profile directory:

```bash
ROOT="$HOME/xr-gate"

SRC="$HOME/xr_calib/$CALIB_TARGET/final/$CALIB_TARGET/$CALIB_UNIT_ID/$CALIB_PROFILE_NAME"


DST="$ROOT/devices/<device_config>/configs/calibration_dataset/final/$CALIB_TARGET/$CALIB_UNIT_ID/$CALIB_PROFILE_NAME"
#Example
#DST="$ROOT/devices/leap_motion_uvc_nrf54l15/configs/calibration_dataset/final/$CALIB_TARGET/$CALIB_UNIT_ID/$CALIB_PROFILE_NAME"

mkdir -p "$DST"

```

Install:

```bash
install -m 0644 \
  "$SRC/basalt_calib_${CALIB_PROFILE_NAME}.json" \
  "$DST/basalt_calib_640x480.json"

install -m 0644 \
  "$SRC/mercury_calib_${CALIB_PROFILE_NAME}.json" \
  "$DST/mercury_calib_640x480.json"

install -m 0644 \
  "$SRC/basalt_vio_config_${CALIB_PROFILE_NAME}.json" \
  "$DST/basalt_vio_config_640x480.json"
  
install -m 0644 \
  "$SRC/basalt_vo_config_${CALIB_PROFILE_NAME}.json" \
  "$DST/basalt_vo_config_640x480.json"  
```

All three source files are now produced by `convert-runtime`; no separate VIO
config creation step is required.

Point the runtime environment to:
Create 

```bash
ROOT="$HOME/xr-gate"

TRACKING_SENSOR="device"

#Example
#TRACKING_SENSOR="leap_motion_uvc_nrf54l15"
CALIB_TARGET="my_stereo_imu"
CALIB_UNIT_ID="mount_v1_unit_001"
CALIB_PROFILE_NAME="stereo_640x480_none"

CAPTURE_NAMESPACE="tracker"
#Example
#CAPTURE_NAMESPACE="leap_uvc_test"

CAPTURE_CONFIG_NAME="device_profile.yaml"
#Example
#CAPTURE_CONFIG_NAME="leap_motion_uvc_nrf54l15.yaml"

TRACKING_ENV="$ROOT/devices/$TRACKING_SENSOR/tracking.env"

mkdir -p "$(dirname "$TRACKING_ENV")"

cat > "$TRACKING_ENV" <<EOF
#!/usr/bin/env bash
# Tracking-sensor layer for $TRACKING_SENSOR.
# Loaded after the display device environment by xr_client.

_XR_TRACKING_ENV_DIR="\$(cd "\$(dirname "\${BASH_SOURCE[0]}")" && pwd)"

export XR_TRACKING_SENSOR="$TRACKING_SENSOR"
export XR_TRACKING_HOME="\$_XR_TRACKING_ENV_DIR"
export XR_TRACKING_SENSOR_HOME="\$XR_TRACKING_HOME"
export XR_TRACKING_CONFIGS_ROOT="\$XR_TRACKING_HOME/configs"
export XR_TRACKING_CALIB_DIR="\$XR_TRACKING_CONFIGS_ROOT/calibration_dataset"

# Existing backend launchers use XR_CALIB_DIR as their calibration root.
export XR_CALIB_DIR="\$XR_TRACKING_CALIB_DIR"

export CAPTURE_REGISTRY_NAMESPACE="$CAPTURE_NAMESPACE"
export CAPTURE_SERVICE_CONFIG_PATH="\$XR_TRACKING_CONFIGS_ROOT/capture_service/$CAPTURE_CONFIG_NAME"

export XR_TRACKING_CALIB_TARGET="\${XR_TRACKING_CALIB_TARGET:-$CALIB_TARGET}"
export XR_TRACKING_UNIT_ID="\${XR_TRACKING_UNIT_ID:-$CALIB_UNIT_ID}"
export XR_TRACKING_CALIB_PROFILE="\${XR_TRACKING_CALIB_PROFILE:-$CALIB_PROFILE_NAME}"

export FINAL_PROFILE_DIR="\$XR_TRACKING_CALIB_DIR/final/\$XR_TRACKING_CALIB_TARGET/\$XR_TRACKING_UNIT_ID/\$XR_TRACKING_CALIB_PROFILE"

export BASALT_CALIB="\${XR_TRACKING_BASALT_CALIB:-\$FINAL_PROFILE_DIR/basalt_calib_640x480.json}"
export BASALT_VIO_CONFIG="\${XR_TRACKING_BASALT_VIO_CONFIG:-\$FINAL_PROFILE_DIR/basalt_vio_config_640x480.json}"
export BASALT_VO_CONFIG="\${XR_TRACKING_BASALT_VO_CONFIG:-\$FINAL_PROFILE_DIR/basalt_vo_config_640x480.json}"
export MERCURY_CALIB="\${XR_TRACKING_MERCURY_CALIB:-\$FINAL_PROFILE_DIR/mercury_calib_640x480.json}"

export TRACKING_TRANSFORM_CONFIG="\$XR_TRACKING_CONFIGS_ROOT/xr_runtime_adapter/xr_21_joint_hand_viewer_verified.json"
export XR_SPATIAL_PROFILE_DIR="\$XR_TRACKING_CONFIGS_ROOT/xr_spatial/profiles"
export XR_RUNTIME_DEBUG_VIEWER_CONFIG="\$XR_TRACKING_CONFIGS_ROOT/runtime_debug_viewer/xr_runtime_stock.yaml"

unset _XR_TRACKING_ENV_DIR
EOF

chmod 0644 "$TRACKING_ENV"
bash -n "$TRACKING_ENV"

echo "[OK] created: $TRACKING_ENV"
```

Start `capture_service_cpp` and Basalt first.

Enable Mercury and the rest of the XR stack only after pose tracking works.

---

# Set VO Basalt (For none IMU)

If basalt crash in no-imu mode try set 'optical_flow_epipolar_error' in basalt_vio_config.json:

Enable debug in basalt_vio_config.json:

```text
"config.vio_debug": true
```

Find  the best value `optical_flow_epipolar_error` in interval (0.05, 0.01 ... 0.5 , 0.6 ... 1, 2, ..)
```text
 "config.optical_flow_epipolar_error": 0.7
```

Also recommended set duration in basalt launcher `start_basalt.sh` (For ease of checking, `optical_flow_epipolar_error`):

3 sec run:

```text
--duration 3
```

Example bad log basalt with debug:

```text
connected0 1 unconnected0 128
```

Example good basalt with debug:

```text
connected0 183 unconnected0 128
```

You need to find empirically the value that will give the largest number of `connected0` and at the same time the percentage of connected0 would be the largest relative to the sum of connected0 and unconnected0

If `connected0` does not change or remains near 0 try change `vio_min_triangulation_dist`


```bash
CALIB=/path/to/basalt_calib_stereo.json

python3 - "$CALIB" <<'PY'
import json
import math
import sys

with open(sys.argv[1], encoding="utf-8") as f:
    value = json.load(f)["value0"]

cam0, cam1 = value["T_imu_cam"]

dx = float(cam1["px"]) - float(cam0["px"])
dy = float(cam1["py"]) - float(cam0["py"])
dz = float(cam1["pz"]) - float(cam0["pz"])

baseline = math.sqrt(dx * dx + dy * dy + dz * dz)

for k in (0.5, 0.7, 0.8, 0.9):
    print(f"k={k:.1f}: {baseline * k:.6f} m")

print(f"baseline: {baseline:.6f} m ({baseline * 1000:.2f} mm)")
PY
```

OR
threshold = 0.8 × baseline

```text
config.vio_min_triangulation_dist = 0.8 × baseline
```

Example:

```text
"config.vio_min_triangulation_dist": 0.03,
```


After tests set found "optical_flow_epipolar_error", "config.vio_min_triangulation_dist" and "config.vio_debug": false in basalt_vio_config.json:

---

# Changing the IMU frequency later

If only the IMU frequency changes:

```text
camera calibration can usually be reused
camera–IMU calibration must be repeated
runtime JSON must be regenerated
```

Do the following:

1. Change the IMU ODR in firmware/configuration.
2. Measure the actual published `imu0` rate.
3. Update `IMU_UPDATE_RATE` in `<target>.env`.
4. Regenerate the IMU YAML with `FORCE_TARGET_CONFIGS=1`.
5. Record a new stereo+IMU dataset.
6. Create a new ROS bag.
7. Reuse the existing camera-only calibration if camera geometry is unchanged.
8. Repeat `imu-camera`.
9. Repeat `convert-runtime`.
10. Verify `.value0.imu_update_rate` and `.value0.cam_time_offset_ns`.

Example:

```bash
IMU_UPDATE_RATE=416.0
```

Expected sample count for 90 seconds:

```text
approximately 37440 IMU samples
```

Repeat the IMU axis calibration only if:

```text
the IMU was remounted
firmware changed axis order
firmware changed axis signs
the published IMU frame changed
```

---

# Recalibrate after changing

```text
camera resolution
crop
rotation or flip
left/right order
lens or focus
IMU axis transform
IMU sample rate
camera or IMU timestamp mode
camera–IMU position or angle
```
