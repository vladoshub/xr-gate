# OpenVR Driver

SteamVR/OpenVR driver for the XR runtime streams produced by `xr_runtime_adapter`.

The driver is intentionally thin. It does not read cameras, IMU, Basalt, Mercury, or raw device data. It only consumes runtime-normalized HMD/controller streams.

## Expected runtime streams

```text
/tmp/runtime_tracking_streams.json : runtime_hmd_pose
/tmp/runtime_tracking_streams.json : runtime_controller_state
/tmp/runtime_tracking_streams.json : runtime_hand_tracking        # optional fallback
```

## Package driver directory

Historical XREAL packages keep their existing names, for example:

```text
out/xreal_ultra/bin/drivers/openvr_driver_60HZ/xr_tracking/
```

Custom device profiles are namespaced, for example:

```text
out/my_glasses/bin/drivers/openvr_driver_my_glasses_72HZ/xr_tracking/
```

The final `xr_tracking/` directory is the SteamVR driver package registered manually or with `vrpathreg`.

## Display and optics config

OpenVR build and registration use:

```text
drivers/openvr_driver/configs/display/default.yaml
```

Build with another config:

```bash
XR_OPENVR_DISPLAY_CONFIG=/path/cardboard.yaml \
drivers/openvr_driver/scripts/build_and_register_driver.sh
```

The schema is:

```yaml
display:
  width_px: 3840
  height_px: 1080
  layout: side_by_side_horizontal
  eye_width_px: 1920
  eye_height_px: 1080
  refresh_hz: 60
  rotation_deg: 0

optics:
  ipd_m: 0.064
  inter_lens_distance_m: 0.064
  screen_to_lens_distance_m: 0.0
  eye_to_lens_distance_m: 0.0

  left_eye:
    lens_center_uv: [0.5, 0.5]
    fov_deg:
      left: 45.0
      right: 45.0
      up: 45.0
      down: 45.0

  right_eye:
    lens_center_uv: [0.5, 0.5]
    fov_deg:
      left: 45.0
      right: 45.0
      up: 45.0
      down: 45.0
```

The checked-in default reproduces the previous hardcoded OpenVR package values.
The normalized config is rendered into `default.vrsettings`, and a copy is
stored in the assembled driver package at:

```text
xr_tracking/resources/settings/display_config.yaml
```

Registration uses the rendered settings from that package. Changing the YAML
therefore requires rebuilding the OpenVR package, but not changing driver source.

An explicit frequency override remains supported and has priority over
`display.refresh_hz`:

```bash
XR_OPENVR_DISPLAY_CONFIG=/path/cardboard.yaml \
XR_OPENVR_DISPLAY_FREQUENCY_HZ=72 \
drivers/openvr_driver/scripts/build_and_register_driver.sh
```

Display geometry, layout, rotation, IPD, and asymmetric per-eye FOV are applied
to the OpenVR package. Lens centers, inter-lens distance, and screen/eye-to-lens
distances are loaded into the driver settings; while identity distortion is
active they are optics metadata for the future distortion implementation.


## Device identity overlays

Model/serial and other device-specific settings can still be supplied by:

```text
drivers/openvr_driver/devices/<profile>/settings/default.vrsettings
```

The display/optics YAML is applied after this overlay, so display geometry comes
from the selected YAML config. Existing environment overrides remain available
for one-off tests and have higher priority.

## Package naming

`XR_OPENVR_PACKAGE_TAG` can distinguish packages for the same profile, mode,
and frequency. Existing XREAL package names such as `openvr_driver_60HZ` remain
unchanged.
