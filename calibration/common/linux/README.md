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
