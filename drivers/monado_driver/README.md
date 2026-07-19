# Monado Driver Integration

Project-owned Monado driver integration for consuming runtime streams from
`xr_runtime_adapter`.

The driver does not talk directly to capture, Basalt, Mercury, or device
hardware. It consumes runtime-normalized streams only.

## Expected runtime streams

```text
/tmp/runtime_tracking_streams.json : runtime_hmd_pose
/tmp/runtime_tracking_streams.json : runtime_controller_state
/tmp/runtime_tracking_streams.json : runtime_hand_tracking        # optional fallback
```

## Generic build and package target

The same Monado binary can be packaged for any validated device target:

```bash
XR_MONADO_DEVICE=my_glasses \
BIN_DIR="$PWD/out/my_glasses/bin/drivers/monado_driver" \
XR_BIN_ROOT="$PWD/out/my_glasses/bin" \
drivers/monado_driver/scripts/linux/install.sh
```

`xreal_ultra` remains the default target for backward compatibility. Arbitrary
target names matching `[a-z0-9][a-z0-9_.]*` are accepted. Device-specific helper
scripts are optional; the guaranteed generic entry point is:

```text
bin/drivers/monado_driver/start.sh
```

The installer creates a canonical, device-independent OpenXR runtime manifest:

```text
bin/drivers/monado_driver/openxr_monado_xrgate.json
```

and a sourceable helper:

```text
bin/drivers/monado_driver/openxr_runtime_env.sh
```

For backward compatibility it also creates the historical manifest and helper
under `devices/<target>/linux/scripts/monado_driver/`. The generated
`bin/drivers/monado_driver/env.sh` points at the canonical manifest.

## Display and optics config

Monado loads the display geometry at runtime from:

```text
drivers/monado_driver/configs/display/default.yaml
```

The installed copy is:

```text
bin/drivers/monado_driver/configs/display/default.yaml
```

Select another config without rebuilding Monado:

```bash
XR_MONADO_DISPLAY_CONFIG=/path/cardboard.yaml \
  bin/drivers/monado_driver/start.sh
```

The schema is shared with the OpenVR build scripts:

```yaml
display:
  width_px: 3840
  height_px: 1080
  layout: side_by_side_horizontal
  eye_width_px: 1920
  eye_height_px: 1080
  refresh_hz: 90
  rotation_deg: 0

optics:
  ipd_m: 0.064
  inter_lens_distance_m: 0.064
  screen_to_lens_distance_m: 0.0
  eye_to_lens_distance_m: 0.0

  left_eye:
    lens_center_uv: [0.5, 0.5]
    fov_deg:
      left: 57.29577951308232
      right: 57.29577951308232
      up: 57.29577951308232
      down: 57.29577951308232

  right_eye:
    lens_center_uv: [0.5, 0.5]
    fov_deg:
      left: 57.29577951308232
      right: 57.29577951308232
      up: 57.29577951308232
      down: 57.29577951308232
```

The checked-in default reproduces the previous hardcoded Monado values. Config
loading does not require a rebuild. Explicit `XR_MONADO_*` environment values
remain highest priority, followed by the YAML config, legacy `.env` profile,
and finally compiled defaults.

`width_px`, `height_px`, per-eye render size, layout, rotation, refresh rate,
IPD, and per-eye FOV are applied by the runtime driver. Lens centers,
inter-lens distance, and screen/eye-to-lens distances are loaded and validated
now; while identity distortion is active they are retained as optics metadata
for the future distortion implementation.


## Legacy runtime overrides

Legacy named `.env` profiles and one-off `XR_MONADO_*` overrides remain supported.
For example:

```bash
XR_MONADO_EYE_WIDTH=1600 \
XR_MONADO_EYE_HEIGHT=900 \
XR_MONADO_REFRESH_HZ=72 \
  bin/drivers/monado_driver/start.sh
```

Raw-radian FOV variables remain compatibility overrides.

## Package output

For the historical XREAL target:

```text
out/xr-gate/bin/drivers/monado_driver/
```

For another target, replace `xreal_ultra` with its package name.

Monado is upstream code. Required changes to Monado remain represented as
patches under `drivers/monado_driver/patches/`.
