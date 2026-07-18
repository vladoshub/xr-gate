# Calibration targets

- `xreal_ultra/linux` — existing legacy XREAL calibration flow, unchanged (first iteration).
- `common/linux` — new target-selectable shared calibration tooling.

Example:

```bash
./calibration/common/linux/calibrate.sh --target leap_motion_uvc_nrf54l15 show-target
```
