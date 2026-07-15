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

## Runtime display profiles

Resolution, FOV, IPD, and refresh rate are runtime environment settings. They do
not require rebuilding Monado.

Installed profiles live at:

```text
bin/drivers/monado_driver/profiles/<profile>.env
```

Run a named profile:

```bash
XR_MONADO_DISPLAY_PROFILE=my_glasses \
bin/drivers/monado_driver/start.sh
```

Or use an explicit file:

```bash
XR_MONADO_DISPLAY_PROFILE_FILE=/path/my_glasses.env \
bin/drivers/monado_driver/start.sh
```

One-off settings can be supplied directly:

```bash
XR_MONADO_DEVICE=generic \
XR_MONADO_EYE_WIDTH=1600 \
XR_MONADO_EYE_HEIGHT=900 \
XR_MONADO_FOV_HORIZONTAL_DEG=52 \
XR_MONADO_FOV_VERTICAL_DEG=30 \
XR_MONADO_REFRESH_HZ=60 \
bin/drivers/monado_driver/start.sh
```

The degree-based horizontal and vertical values are complete per-eye FOVs.
Directional half-angles are also supported:

```text
XR_MONADO_FOV_LEFT_DEG
XR_MONADO_FOV_RIGHT_DEG
XR_MONADO_FOV_UP_DEG
XR_MONADO_FOV_DOWN_DEG
```

Existing `XR_MONADO_FOV_LEFT/RIGHT/UP/DOWN` variables remain raw radians and
have higher precedence. Explicit `_RAD` aliases have the highest precedence.

Useful geometry variables:

```text
XR_MONADO_EYE_WIDTH / XR_MONADO_EYE_HEIGHT
XR_MONADO_RENDER_WIDTH / XR_MONADO_RENDER_HEIGHT
XR_MONADO_WINDOW_WIDTH / XR_MONADO_WINDOW_HEIGHT
XR_MONADO_DISPLAY_WIDTH_M / XR_MONADO_DISPLAY_HEIGHT_M
XR_MONADO_IPD_M
XR_MONADO_LENS_VERTICAL_POSITION_M
XR_MONADO_REFRESH_HZ
```

## Package output

For the historical XREAL target:

```text
out/xreal_ultra/bin/drivers/monado_driver/
```

For another target, replace `xreal_ultra` with its package name.

Monado is upstream code. Required changes to Monado remain represented as
patches under `drivers/monado_driver/patches/`.
