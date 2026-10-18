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

## Restarting the tracking stack

On Linux, the driver automatically reconnects all three SHM streams when
`xr_runtime_adapter` or the full XR Gate tracking stack restarts. The Monado
service and its OpenXR devices can stay running. The stream names must remain
the same (`runtime_hmd_pose`, `runtime_controller_state`, `runtime_hand_tracking`).

An unlinked or resized SHM mapping is reopened before reading it, even while
the previous sample is fresh. Sequence counters may restart at 1. A stream
whose sequence stops advancing is reopened after 500 ms; missing or partially
initialized objects are retried at most every 100 ms, without a background
thread. Healthy mappings stay open. Existing sample-age checks still reject
stale poses and inputs: reconnecting never refreshes their timestamps.

While tracking is unavailable, controllers and optical hands become inactive;
the HMD keeps the existing identity-pose fallback. Restored tracking resumes on
the existing devices. A full VIO restart can reset the tracking origin, so the
application may need recentering. Reconnection does not preserve the previous
world map or controller IMU alignment state. If Monado itself is stopped by
your launcher, its OpenXR session must still be started again.

This requires rebuilding/installing Monado once after applying the patch.
Capture, tracking backends, SteamVR and calibration files need no changes.

## Controllers and optional OpenXR hands

Controllers use the Touch interaction profile, including A/B/X/Y, analog
trigger/squeeze and touch state. Simple-controller, Vive Wand and WMR binding
profiles remain available. The Vive trackpad compatibility path uses the same
Gear VR touch/motion/D-pad activation rules as the OpenVR driver. SteamVR sources
are not modified by this integration.

Both controller poses and the optional hand fallback are already in runtime
coordinates. The driver copies position, orientation and valid velocities
without applying the HMD transform a second time. A fresh
`runtime_controller_state` frame is authoritative even if it explicitly marks a
controller pose or input invalid.

Native `XR_EXT_hand_tracking` publication is **disabled by default**. Enable it
when starting (or restarting) the Monado service:

```bash
XR_MONADO_HAND_TRACKING=1 bin/drivers/monado_driver/start.sh
```

Leave the variable unset, or set `XR_MONADO_HAND_TRACKING=0`, to expose only
controllers. The switch changes device capabilities at service startup; it does
not require restarting the tracking backend. The normal controller/IMU path
continues in either mode. The startup log reports the switch and each
controller's hand-tracking capability.

The opt-in hand devices consume `runtime_hand_tracking` alongside controllers.
They expose 26 OpenXR joints using the existing 21-joint runtime data: measured
wrist/finger joints are mapped directly, the palm uses its runtime position and
orientation from `palm_*`, and the four missing finger metacarpals are
estimated between wrist and proximal joint. Estimated poses are valid but are
not marked as directly tracked. Zero/missing radii receive conservative defaults.
The driver preserves the runtime's palm orientation, including configured
offsets; any controller-only presentation correction belongs in the runtime.
Invalid palm quaternions clear palm orientation validity without substituting
the wrist orientation or discarding otherwise valid finger joints.
Per-joint velocities are unavailable in the
runtime contract and remain invalid; controller velocities are still passed
through normally.

Hands become inactive on no-hands, degraded/held tracking, missing joints,
expired optical source timestamps or a missing/stale hand stream. IMU-only
controller prediction never fabricates finger tracking. Invalid individual
joints are returned with cleared validity flags and finite placeholder poses.

After applying source changes, rebuild/install Monado using the normal installer:

```bash
XR_MONADO_DEVICE=xreal_ultra drivers/monado_driver/scripts/linux/install.sh
```

It installs the updated builder integration that registers the optional hand roles;
copying only `xr_monado_driver.cpp` into an old Monado checkout is insufficient.
Existing output-directory overrides such as `BIN_DIR` remain supported.

## Driver regression checks

The Linux transport tests use real SHM objects and need no Monado checkout:

```bash
bash drivers/monado_driver/tests/run_posix_shm_tests.sh
```

They cover publisher replacement while the previous frame is still fresh,
sequence reset, stopped/resumed streams, missing publishers, resized/truncated
segments, incomplete/invalid headers and repeated restarts without FD leaks.

The callback tests compile against the same Monado headers used for the build:

```bash
MONADO_DIR="$PWD/third_party/monado_driver" \
  bash drivers/monado_driver/tests/run_tests.sh
```

Set `NLOHMANN_JSON_INCLUDE_DIR` if the JSON headers are outside the default
compiler search path. Add `SANITIZE=1` for AddressSanitizer/UndefinedBehaviorSanitizer.
These tests replace device allocation/debug/display helpers and incoming samples;
they exercise the real input, pose and hand callbacks, including concurrent
calls, default-off capabilities, analog/button handling and tracking loss.
They do not replace an OpenXR application test with physical devices.

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
