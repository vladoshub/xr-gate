# Target-based XR calibration tools

This directory adds a target-selectable calibration flow without changing the legacy
`calibration/xreal_ultra/linux` scripts.

Supported targets:

```text
xreal_ultra
leap_motion_uvc_nrf54l15
```

Use the common entry point:

```bash
cd ~/src/xr_tracking
./calibration/common/linux/calibrate.sh --target leap_motion_uvc_nrf54l15 show-target
```

Commands:

```text
install
record
bag
camera
save-camera
imu-camera
convert-runtime
```

The target profile controls expected image dimensions, capture-service config,
recording prefix, camera model, calibration output directories and IMU YAML defaults.
All values may still be overridden through environment variables.

The legacy XREAL scripts remain available and unchanged under:

```text
calibration/xreal_ultra/linux
```

## Shared calibration defaults

Camera calibration accepts target-independent overrides:

```bash
BAG_FREQ=4.0 APPROX_SYNC=0.005 \
  ./calibration/common/linux/calibrate.sh --target <target> camera
```

Every target profile must define the actual published IMU rate and noise values:

```bash
IMU_UPDATE_RATE=208.0
IMU_ACCEL_NOISE_DENSITY=0.01
IMU_ACCEL_RANDOM_WALK=0.001
IMU_GYRO_NOISE_DENSITY=0.001
IMU_GYRO_RANDOM_WALK=0.0001
```

`convert-runtime` forwards these values into the Basalt/Mercury JSON and rejects
unsupported Kalibr camera models. The current converter supports only the
`pinhole-equi` Kalibr model (`pinhole` + `equidistant` in camchain YAML), which
is emitted as a runtime `kb4` camera.

After changing an existing target's IMU settings, regenerate its IMU YAML:

```bash
FORCE_TARGET_CONFIGS=1 \
  ./calibration/common/linux/calibrate.sh --target <target> install
```
