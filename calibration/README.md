# Calibration targets

- `xreal_ultra/linux` — existing legacy XREAL calibration flow, unchanged.
- `common/linux` — new target-selectable shared calibration tooling.
- `leap_motion_uvc_nrf54l15/linux` — wrappers and instructions for Leap Motion UVC + nRF54L15.

Example:

```bash
./calibration/common/linux/calibrate.sh --target leap_motion_uvc_nrf54l15 show-target
```
