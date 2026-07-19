# Stereo Camera + IMU Calibration — Actions

This guide assumes that the common calibration patch has already been applied.

Required streams:

```text
camera0 — left GRAY8 image
camera1 — right GRAY8 image
imu0    — gyro in rad/s, accelerometer in m/s²
```

The stereo camera and IMU must be rigidly mounted.

---

## 1. Create a target profile

Create:

```text
calibration/common/linux/profiles/<target>.env
```

Example:

```bash
CALIB_TARGET_NAME="my_stereo_imu"
CALIB_LABEL="My Stereo Camera + IMU"
CALIB_DEVICE_NAME="my_stereo_imu"

CALIB_UNIT_ID="${CALIB_UNIT_ID:-mount_v1_unit_001}"
CALIB_PROFILE_NAME="${CALIB_PROFILE_NAME:-stereo_640x480_none}"

EXPECT_WIDTH=640
EXPECT_HEIGHT=480
RECORD_PREFIX="my_stereo_imu_640x480_calib"

CAPTURE_CONFIG_CAMERA_ONLY="${CAPTURE_CONFIG_CAMERA_ONLY:-$ROOT_PROJECT/devices/my_device/configs/capture_service/my_camera_only.yaml}"
CAPTURE_CONFIG_STEREO_IMU="${CAPTURE_CONFIG_STEREO_IMU:-$ROOT_PROJECT/devices/my_device/configs/capture_service/my_stereo_imu.yaml}"
CAPTURE_CONFIG="$CAPTURE_CONFIG_STEREO_IMU"

CAMERA_MODEL_0="pinhole-equi"
CAMERA_MODEL_1="pinhole-equi"

IMU_YAML_NAME="imu_my_sensor.yaml"

# Replace with the measured published imu0 rate.
IMU_UPDATE_RATE=200.0

# Initial values. Replace with measured values when available.
IMU_ACCEL_NOISE_DENSITY=0.01
IMU_ACCEL_RANDOM_WALK=0.001
IMU_GYRO_NOISE_DENSITY=0.001
IMU_GYRO_RANDOM_WALK=0.0001

DEFAULT_RECORD_MODE="stereo_imu"
```

Use a unique `CALIB_UNIT_ID` for every physical assembly or mount revision.

---

## 2. Create the capture-service config

The final stereo+IMU config must publish:

```text
camera0
camera1
imu0
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

Check the generated IMU YAML:

```bash
cat "$HOME/xr_calib/$CALIB_TARGET"/imu*.yaml
```

It must contain:

```yaml
rostopic: /imu0
update_rate: <actual published rate>
```

---

## 4. Calibrate the IMU mount axes

Temporarily remove any existing:

```yaml
imu:
  transform:
```

Start `capture_service_cpp`.

Find the calibration tool:

```bash
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

## 5. Measure and set the real IMU rate

Use an existing `imu.csv` or make a short test recording.

```bash
DS="/path/to/dataset"

N=$(( $(wc -l < "$DS/imu.csv") - 1 ))
FIRST_NS="$(awk -F, 'NR==2 {print $1}' "$DS/imu.csv")"
LAST_NS="$(awk -F, 'END {print $1}' "$DS/imu.csv")"

python3 - <<PY
n = int("$N")
duration = (int("$LAST_NS") - int("$FIRST_NS")) / 1e9
print("measured_hz:", (n - 1) / duration)
PY
```

Set the measured nominal rate in the target profile:

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

```yaml
tagSize: <metres>
tagSpacing: <gap divided by tag size>
```

---

## 7. Record one stereo+IMU dataset

Use one recording for both stereo calibration and camera–IMU calibration:

```bash
RECORD_MODE=stereo_imu \
START_CAPTURE_SERVICE=1 \
SECONDS_TOTAL=90 \
"$CAL_COMMON/calibrate.sh" \
  --target "$CALIB_TARGET" \
  record
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

The bag must contain:

```text
/cam0/image_raw
/cam1/image_raw
/imu0
```

Do not continue if `/imu0` is missing.

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

## 10. Run camera–IMU calibration

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

```bash
"$CAL_COMMON/calibrate.sh" \
  --target "$CALIB_TARGET" \
  convert-runtime
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
DST="$ROOT/devices/<device>/configs/calibration_dataset/final/$CALIB_TARGET/$CALIB_UNIT_ID/$CALIB_PROFILE_NAME"

mkdir -p "$DST"
```

Install:

```bash
install -m 0644 \
  /path/to/basalt_calib_<profile>.json \
  "$DST/basalt_calib_640x480.json"

install -m 0644 \
  /path/to/mercury_calib_<profile>.json \
  "$DST/mercury_calib_640x480.json"

install -m 0644 \
  /path/to/generated/basalt_vio_config_640x480.json \
  "$DST/basalt_vio_config_640x480.json"
```

All three source files are now produced by `convert-runtime`; no separate VIO
config creation step is required.

Point the runtime environment to:

```bash
FINAL_PROFILE_DIR="$DST"
BASALT_CALIB="$DST/basalt_calib_640x480.json"
BASALT_VIO_CONFIG="$DST/basalt_vio_config_640x480.json"
MERCURY_CALIB="$DST/mercury_calib_640x480.json"
CAPTURE_SERVICE_CONFIG_PATH="/path/to/my_stereo_imu.yaml"
```

Start `capture_service_cpp` and Basalt first.

Enable Mercury and the rest of the XR stack only after pose tracking works.

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
