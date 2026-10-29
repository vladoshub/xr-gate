# Monado Driver Integration

Project-owned Monado driver integration for consuming runtime streams from
`xr_runtime_adapter`.

The driver does not talk directly to capture, Basalt, Mercury, or device
hardware. It consumes runtime-normalized streams only.

## Optional spatial, video and TCP

See [the setup and API guide](docs/SPATIAL_VIDEO_TCP.ru.md).
SHM remains the default. `XR_MONADO_TRANSPORT=tcp` selects the application's
existing runtime TCP protocol with reconnect. `XR_MONADO_SPATIAL=1` enables
`XR_EXT_spatial_entity`, mesh tracking and archetypes; `XR_MONADO_VIDEO=1`
enables exact stereo CPU frame access through the experimental
`XR_MNDX_xr_gate_data` extension. Both optional data paths default to off.
The app and service must receive the same transport/data settings.

HMD invalid-to-valid transitions trigger native Monado LOCAL/LOCAL_FLOOR
recenter on the next `xrWaitFrame` by default. Set
`XR_MONADO_RECENTER_ON_REACQUIRE=0` to disable it. A continuously valid pose's
`reset_counter` change alone does not trigger recenter.

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
the existing devices. A full VIO restart can reset the tracking origin. The default
invalid-to-valid recenter above handles reacquisition in LOCAL/LOCAL_FLOOR. Reconnection does not preserve the previous
world map or controller IMU alignment state. If Monado itself is stopped by
your launcher, its OpenXR session must still be started again.

This requires rebuilding/installing Monado once after applying the patch.
Capture, tracking backends, SteamVR and calibration files need no changes.

## Controllers and optional OpenXR hands

Controllers use the Touch interaction profile, including A/B/X/Y, analog
trigger/squeeze and touch state. Alternative profiles default to Index, WMR,
Vive, then Simple. Monado tries native Touch first when the app supplies it,
then the first available alternative in this order. Simple no longer hides a
richer available profile. Index exposes button/stick/pad/pose compatibility;
it does not add skeletal fingers or physical haptics.

Monado and OpenVR share the generic input conversion in
`shared/include/xr_runtime/input/controller_input_mapping.hpp`. Capacitive touch
and direction do not imply a pad click. The physical switch drives pad/stick
click; the legacy touch-to-click behavior is opt-in. A/B/X/Y keep their runtime
identities by default. Assign physical buttons in the controller's input config:
the Gear VR template maps left volume up/down to X/Y and right volume up/down to
A/B. The Index compatibility profile reads left X/Y as its A/B buttons without
duplicating native Touch inputs. Legacy left A/X and B/Y aliases are opt-in.
Menu and System remain separate inputs. Trigger/grip analog values survive
digital click flags; digital-only presses supply a full pull. Input flags and
pose validity are independent.

`start.sh` loads `profiles/controller_input.env` for every hardware/display
profile. Use `XR_MONADO_CONTROLLER_INPUT_CONFIG=/path/to/controller_input.env`
for another file. Explicit environment overrides work with the supplied file.
The driver also has the same defaults when starting `monado-service` directly.

| Variable | Default | Purpose |
| --- | --- | --- |
| `XR_MONADO_CONTROLLER_LEFT_AB_XY_ALIAS` | `0` | Preserve independent inputs; use `1` only for legacy left A/X and B/Y aliases. |
| `XR_MONADO_CONTROLLER_TRACKPAD_CLICK_FROM_TOUCH` | `0` | Restore the old synthetic click for custom compatibility bindings. |
| `XR_MONADO_CONTROLLER_AXIS_THRESHOLD` | `0.05` | Neutral axis region and virtual touch activity threshold. |
| `XR_MONADO_CONTROLLER_CLICK_THRESHOLD` | `0.55` | Analog trigger/grip click threshold. |
| `XR_MONADO_CONTROLLER_EXPOSE_SYSTEM_BUTTON` | `1` | Publish the existing System input; it does not press application Menu. |
| `XR_MONADO_CONTROLLER_PROFILE_ORDER` | `index,wmr,vive,simple` | Alternative profile order; native Touch remains first. |

The order accepts a comma-separated subset of those four names, without
duplicates. Invalid lists fall back to the default with a warning. Each device
keeps its own ordered binding table. Index squeeze force follows normalized
grip value and trackpad force is binary physical-switch state; neither measures
actual pressure. In Vive/WMR games, actions bound to pad click require a physical
press by default. See [the Russian controller guide](docs/CONTROLLER_INPUT.ru.md).

Existing saved Gear VR profiles are not overwritten by a template update. Stop
the controller service, then run this once from the source checkout (a backup is
created; device identities, calibration and other bindings are preserved):

```bash
python3 override_controller/scripts/linux/migrate_gearvr_face_buttons.py \
  ~/.config/xr_tracking/override_controller/gearvr.json
```

Pass the actual selected profile path if it differs. Older custom or gesture
profiles which still emit left A/B must be updated at their input source or use
`XR_MONADO_CONTROLLER_LEFT_AB_XY_ALIAS=1` explicitly. A previously exported `1`
also overrides the new default; unset it or set it to `0` for independent inputs.

Both controller poses and the optional hand fallback are already in runtime
coordinates. The driver copies position, orientation and valid velocities
without applying the HMD transform a second time. A fresh
`runtime_controller_state` frame is authoritative even if it explicitly marks a
controller pose or input invalid.

Native `XR_EXT_hand_tracking` publication is **disabled by default**. Enable it
when starting (or restarting) the Monado service:

```bash
XR_MONADO_HAND_TRACKING=1 bin/drivers/generic/monado_driver/start.sh
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
calls, default-off capabilities, touch/click separation, per-hand button aliases,
configurable thresholds/profile order, Index mapping, gesture fallback and input
release on stream loss. The existing functional-test workflow runs these checks
against the Monado revision pinned by `install.sh`.
They do not replace an OpenXR application test with physical devices.

## Generic build and package target

The same Monado binary can be packaged for any validated device target:

```bash
XR_MONADO_DEVICE=my_glasses \
BIN_DIR="$PWD/out/my_glasses/bin/drivers/my_glasses/monado_driver" \
XR_BIN_ROOT="$PWD/out/my_glasses/bin" \
drivers/monado_driver/scripts/linux/install.sh
```

`xreal_ultra` remains the default target for backward compatibility. Arbitrary
target names matching `[a-z0-9][a-z0-9_.]*` are accepted. Device-specific helper
scripts are optional; the guaranteed generic entry point is:

```text
bin/drivers/generic/monado_driver/start.sh
```

Each device package contains its own manifest with a relative library path:

```text
bin/drivers/generic/monado_driver/openxr_monado_xrgate.json
```

and a sourceable helper:

```text
bin/drivers/generic/monado_driver/openxr_runtime_env.sh
```

For backward compatibility it also creates the historical manifest and helper
under `devices/<target>/linux/scripts/monado_driver/`. The generated
`bin/drivers/generic/monado_driver/env.sh` points at the canonical manifest.

## Display and optics config

Monado loads the display geometry at runtime from:

```text
drivers/monado_driver/configs/display/default.yaml
```

The installed copy is:

```text
bin/drivers/generic/monado_driver/configs/display/default.yaml
```

Select another config without rebuilding Monado:

```bash
XR_MONADO_DISPLAY_CONFIG=/path/cardboard.yaml \
  bin/drivers/generic/monado_driver/start.sh
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
  bin/drivers/generic/monado_driver/start.sh
```

Raw-radian FOV variables remain compatibility overrides.

## Package output

For the historical XREAL target:

```text
out/xr-gate/bin/drivers/generic/monado_driver/
```

For another target, replace `xreal_ultra` with its package name.

Monado is upstream code. Required changes to Monado remain represented as
patches under `drivers/monado_driver/patches/`.

Device folders and profile discovery: [DEVICE_PACKAGES.md](../DEVICE_PACKAGES.md).
