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

## Generic display profiles

The driver binary is hardware-neutral. Resolution, per-eye render size, FOV,
refresh rate, model identity, and display mode are written into the assembled
SteamVR package settings.

A named profile is a normal settings overlay:

```text
drivers/openvr_driver/devices/<profile>/settings/default.vrsettings
```

Build and register it with:

```bash
XR_OPENVR_DEVICE=my_glasses \
XR_OPENVR_DISPLAY_FREQUENCY_HZ=72 \
drivers/openvr_driver/scripts/build_and_register_driver.sh
```

Profile names are normalized to lowercase with `-` replaced by `_`. Arbitrary
names matching `[a-z0-9][a-z0-9_.]*` are accepted. `xreal_air2ultra` remains an
alias for `xreal_ultra`.

For one-off builds, use `generic` and override geometry without creating a file:

```bash
XR_OPENVR_DEVICE=generic \
XR_OPENVR_EYE_WIDTH=1600 \
XR_OPENVR_EYE_HEIGHT=900 \
XR_OPENVR_FOV_HORIZONTAL_DEG=52 \
XR_OPENVR_FOV_VERTICAL_DEG=30 \
XR_OPENVR_DISPLAY_FREQUENCY_HZ=60 \
drivers/openvr_driver/scripts/build_and_register_driver.sh
```

`XR_OPENVR_FOV_HORIZONTAL_DEG` and `XR_OPENVR_FOV_VERTICAL_DEG` are complete
per-eye FOV values. Asymmetric directional half-angles are supported with:

```text
XR_OPENVR_FOV_LEFT_DEG
XR_OPENVR_FOV_RIGHT_DEG
XR_OPENVR_FOV_UP_DEG
XR_OPENVR_FOV_DOWN_DEG
```

Raw OpenVR projection tangents remain available and have highest precedence:

```text
XR_OPENVR_PROJECTION_LEFT
XR_OPENVR_PROJECTION_RIGHT
XR_OPENVR_PROJECTION_TOP
XR_OPENVR_PROJECTION_BOTTOM
```

Other useful overrides:

```text
XR_OPENVR_WINDOW_X / XR_OPENVR_WINDOW_Y
XR_OPENVR_WINDOW_WIDTH / XR_OPENVR_WINDOW_HEIGHT
XR_OPENVR_RENDER_WIDTH / XR_OPENVR_RENDER_HEIGHT
XR_OPENVR_MODEL_NUMBER / XR_OPENVR_SERIAL_NUMBER
XR_OPENVR_IPD_M
XR_OPENVR_PACKAGE_TAG
```

`XR_OPENVR_PACKAGE_TAG` distinguishes two packages for the same profile, mode,
and frequency, for example different FOV experiments. Existing XREAL package
names such as `openvr_driver_60HZ` are preserved for compatibility. Custom
profiles are namespaced automatically, for example:

```text
openvr_driver_my_glasses_72HZ
```

Display frequency accepts finite values from 1 to 1000 Hz, including fractional
values such as `59.94`. The physical display/adapter must still expose the
selected mode.
