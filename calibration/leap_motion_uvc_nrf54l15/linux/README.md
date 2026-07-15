# Leap Motion UVC + nRF54L15 calibration flow

This target calibrates a rigid assembly consisting of:

```text
Leap Motion Controller in UVC mode
+
XIAO nRF54L15 Sense / LSM6DS3TR-C IMU
+
a repeatable rigid mount
```

The implementation uses project-owned generic calibration scripts. It does not
include or redistribute LeapUVC firmware, firmware tools, SDK code or calibration
code from third-party repositories.

The existing XREAL Ultra calibration flow is not modified. These wrappers simply
select the `leap_motion_uvc_nrf54l15` target from `calibration/common/linux`.

## Expected runtime streams

```text
camera0: 640x480 GRAY8
camera1: 640x480 GRAY8
imu0:    gyro rad/s + acceleration m/s^2
```

The default camera profile assumes the runtime capture config applies the same image
orientation used in production, currently `640x480` with both Leap images rotated
180 degrees. Calibration must use exactly the same crop, rotation, flip and resolution
as the runtime profile.

## 0. Inspect resolved settings

```bash
cd ~/src/xr_tracking/calibration/leap_motion_uvc_nrf54l15/linux
./scripts/show_target.sh
```

By default the target expects these source-tree configs:

```text
capture_service_cpp/configs/ultraleap_uvc.yaml
capture_service_cpp/configs/ultraleap_uvc_nrf54l15.yaml
```

Override either path when necessary:

```bash
CAPTURE_CONFIG_CAMERA_ONLY=/path/to/leap_camera_only.yaml \
./scripts/start_record.sh
```

## 1. Install Kalibr environment

```bash
cd ~/src/xr_tracking/calibration/leap_motion_uvc_nrf54l15/linux
./scripts/install_kalibr.sh
```

The target work directory defaults to:

```text
~/xr_calib/leap_motion_uvc_nrf54l15
```

Datasets default to:

```text
~/xr_records/leap_motion_uvc_nrf54l15
```

## 2. Camera-only calibration — available before nRF54L15 arrives

Record stereo images without requiring `imu0`:

```bash
cd ~/src/xr_tracking/calibration/leap_motion_uvc_nrf54l15/linux
RECORD_MODE=camera_only \
START_CAPTURE_SERVICE=1 \
SECONDS_TOTAL=90 \
./scripts/start_record.sh
```

The recorder starts `capture_service_cpp` with `ultraleap_uvc.yaml`, subscribes only
to `camera0` and `camera1`, and writes an empty-header `imu.csv` intentionally.

Convert the latest dataset to a camera-only ROS bag:

```bash
./scripts/run_conversion_to_ros.sh
```

Run stereo calibration:

```bash
./scripts/run_kalibr_camera.sh
./scripts/save_camera_calibration.sh
```

The installed camera-only camchain is written under:

```text
~/xr_calib/leap_motion_uvc_nrf54l15/camera/stereo_640x480_rot180/
```

## 3. Prepare the final rigid mount

The camera-IMU transform is transferable only when every assembly uses the same
mechanical reference:

- identical Leap orientation;
- identical nRF54L15 board orientation;
- fixed translation between both devices;
- no flex during recording or use.

Set a distinct unit or mount identifier when desired:

```bash
CALIB_UNIT_ID=reference_mount_v1_unit_001 ./scripts/show_target.sh
```

## 4. Stereo+IMU recording — after nRF54L15 firmware is available

The nRF firmware must publish `imu0` through the capture service using the same
axis convention, units, ODR and timestamp implementation that will be used at runtime.

```bash
RECORD_MODE=stereo_imu \
START_CAPTURE_SERVICE=1 \
SECONDS_TOTAL=90 \
./scripts/start_record.sh
```

This mode uses:

```text
capture_service_cpp/configs/ultraleap_uvc_nrf54l15.yaml
```

Convert the dataset:

```bash
./scripts/run_conversion_to_ros.sh
```

## 5. IMU noise parameters

The generated file:

```text
~/xr_calib/leap_motion_uvc_nrf54l15/imu_nrf54l15_lsm6ds3trc_preliminary.yaml
```

contains preliminary placeholders. Before treating the final camera-IMU calibration
as verified, replace them with values measured from the final board, firmware, ODR,
full-scale and mounting configuration, preferably using an Allan-variance recording.

## 6. Camera-IMU calibration

Use the previously installed camera-only camchain:

```bash
FROM=3 \
TO=60 \
MAX_ITER=6 \
TIMEOFFSET_PADDING=0.5 \
./scripts/run_kalibr_imu_camera.sh
```

The final profile defaults to:

```text
~/xr_calib/leap_motion_uvc_nrf54l15/final/
  leap_motion_uvc_nrf54l15/
    reference_mount_v1/
      stereo_640x480_rot180/
```

## 7. Convert to Basalt and Mercury JSON

```bash
./scripts/convert_runtime_json.sh
```

This creates:

```text
basalt_calib_stereo_640x480_rot180.json
mercury_calib_stereo_640x480_rot180.json
```

## Direct target selection

The wrappers are optional. The same commands can be run centrally:

```bash
cd ~/src/xr_tracking
./calibration/common/linux/calibrate.sh \
  --target leap_motion_uvc_nrf54l15 \
  record
```
