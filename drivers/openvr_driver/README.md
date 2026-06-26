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

```text
out/xreal_ultra/bin/drivers/openvr_driver/xr_tracking/
```

That `xr_tracking/` directory is the SteamVR driver package registered with `vrpathreg`.
