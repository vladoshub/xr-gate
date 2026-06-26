# Mercury hand tracking backend layout

Current canonical layout:

```text
backends/mercury_hand_tracking/
  CMakeLists.txt
  src/capture_hand_tracking_backend.cpp
  include/mercury_hand_tracking/mercury_runtime_loader.hpp
  monado_overlay/src/xrt/tracking/hand/mercury/...
  patches/monado/mercury_xr_upstream_changes.patch
  scripts/linux/...
  tools/debug/...

shared/include/
  capture_client/
  xr_runtime/
```

`mercury_runtime_loader.hpp` is backend-specific and no longer lives under `shared/include/xr_tracking`.

Default Mercury calibration path:

```text
~/src/xr_tracking/calibration_dataset/final/xreal_air2ultra/ZBBM5DZFMP/unified_480_ccw90/mercury_calib_unified_480_ccw90.json
```
