# xr_runtime_adapter

`xr_runtime_adapter` is the runtime-side bridge for XR Gate. It reads backend tracking streams, applies runtime coordinate transforms and stability filters, then republishes normalized runtime streams for drivers and tools.

## What it does

* Reads HMD pose from backend tracking streams.
* Reads hand tracking frames from Mercury or compatible hand backends.
* Optionally reads controller input override streams.
* Optionally reads body tracker streams.
* Applies runtime transform configuration.
* Applies hand stability, reacquire, lost-pose hold/prediction, and gesture derivation.
* Publishes runtime SHM streams consumed by OpenVR/Monado drivers and debug tools.

Typical data flow:

```text
backend streams
  -> xr_runtime_adapter
  -> runtime streams
  -> OpenVR / Monado / debug viewers
```

## Main runtime streams

Common output streams:

```text
runtime_pose
runtime_hand_tracking
runtime_controller_state
runtime_body_trackers
runtime_spatial_proxy_mesh
runtime_spatial_summary
```

Runtime streams are usually registered in:

```text
/tmp/runtime_tracking_streams.json
```

Source/backend streams are usually registered in:

```text
/tmp/tracking_streams.json
```

## Build

From the repository root:

```bash
runtime_adapters/xr_runtime_adapter/scripts/linux/install_xr_runtime_adapter.sh
```

## Run

Default SHM runtime adapter launcher:

```bash
runtime_adapters/xr_runtime_adapter/scripts/linux/start_xr_runtime_adapter_shm.sh
```

The script is controlled mostly through environment variables, so individual runtime features can be enabled without changing code.

## HMD mounting orientation calibration

`orientation_transform` converts the source quaternion into the runtime
coordinate basis. A separate per-stream `orientation_offset` applies the fixed
sensor-to-HMD mounting correction after that conversion.

For a new external sensor, use the guided calibration. It records a neutral
pose followed by one-way pitch-up, yaw-right, and roll-right motions. This
recovers the complete local sensor-to-HMD rotation and detects sign errors that
a single neutral pose cannot observe:

```bash
runtime_adapters/xr_runtime_adapter/tools/calibrate_hmd_orientation_offset.py \
  --config devices/leap_motion_uvc_nrf54l15/configs/xr_runtime_adapter/xr_21_joint_hand_viewer_verified.json \
  --mode guided \
  --replace-existing-offset \
  --write
```

During every motion phase, rotate once in the requested direction and hold that
pose. Return to the same straight neutral pose only when the next neutral phase
starts. Guided mode writes only `orientation_offset`; it does not modify
`orientation_transform` or positional axis mapping.

The default target list is `hmd,hmd_3dof`. The script creates a timestamped
backup and atomically writes the same post-multiply quaternion to both blocks.
Use `--targets hmd` or `--targets hmd_3dof` when the streams come from different
physical sensors.

Packaged builds expose the same tool as:

```bash
./calibrate_hmd_orientation_offset.sh \
  --config devices/leap_motion_uvc_nrf54l15/configs/xr_runtime_adapter/xr_21_joint_hand_viewer_verified.json \
  --mode guided \
  --replace-existing-offset \
  --write
```

After writing the config, restart `xr_runtime_adapter`. Verify the stored offset
and all three rotation directions without changing the file:

```bash
./calibrate_hmd_orientation_offset.sh \
  --config devices/leap_motion_uvc_nrf54l15/configs/xr_runtime_adapter/xr_21_joint_hand_viewer_verified.json \
  --mode guided \
  --verify
```

The older `--mode level` and `--mode full-neutral` modes remain available for a
quick static neutral correction. They cannot determine pitch/yaw/roll signs and
therefore are not sufficient as the only calibration for a new sensor mount.

## Hand tracking stability

Hand stability gate can smooth short tracking losses and reject unstable reacquire jumps.

Useful settings:

```bash
RUNTIME_HAND_STABILITY_GATE=1
RUNTIME_HAND_GATE_HOLD_LOST_MS=50
RUNTIME_HAND_GATE_PREDICT_LOST_MS=350
RUNTIME_HAND_GATE_PREDICTION_DAMPING=0.5
RUNTIME_HAND_GATE_MAX_PREDICTION_VELOCITY_MPS=2.0
RUNTIME_HAND_GATE_PUBLISH_PREDICTED_VELOCITY=0
RUNTIME_HAND_GATE_REACQUIRE_BLEND_MS=0
```

Set `RUNTIME_HAND_GATE_PUBLISH_PREDICTED_VELOCITY=1` only when the downstream runtime should receive the decaying synthetic linear velocity as well as the already predicted pose. The default `0` avoids downstream double prediction. Angular velocity remains zero because hand orientation is held during the lost-hand prediction.

For immediate inertial prediction after hand loss:

```bash
RUNTIME_HAND_GATE_HOLD_LOST_MS=0
RUNTIME_HAND_GATE_PREDICT_LOST_MS=400
RUNTIME_HAND_GATE_PREDICTION_DAMPING=0.8
```

For safer behavior with less hand drift:

```bash
RUNTIME_HAND_GATE_HOLD_LOST_MS=100
RUNTIME_HAND_GATE_PREDICT_LOST_MS=300
RUNTIME_HAND_GATE_PREDICTION_DAMPING=0.35
RUNTIME_HAND_GATE_MAX_PREDICTION_VELOCITY_MPS=0.8
```

Optional rolling-window velocity estimation can replace the legacy final-frame
velocity source without changing the hold/predict/reacquire state machine:

```bash
RUNTIME_HAND_TRACKING_PREDICTION_WINDOW_MODE=1
RUNTIME_HAND_TRACKING_PREDICTION_WINDOW_MS=500
```

The estimator keeps accepted real controller positions from the last window and
uses a least-squares position-vs-time slope when tracking is lost. Synthetic
predicted or reacquire-blended positions are not used as input samples.

## Derived gestures

The adapter can ignore backend-provided hand gestures and derive runtime gestures from hand pose data.

```bash
RUNTIME_IGNORE_BACKEND_HAND_GESTURES=1
RUNTIME_DERIVE_HAND_GESTURES=1
```

Common grab tuning:

```bash
DERIVED_GRAB_ACTIVE_THRESHOLD=0.99
DERIVED_GRAB_DEACTIVE_THRESHOLD=0.65
DERIVED_GRAB_RESPONSE_START=0.85
```

## Controller input override

The adapter can merge hand tracking with external controller input. This is useful when hand pose is available, but buttons, triggers, sticks, or fallback input should come from an external controller.

When an HMD-relative lost-hand fallback is active together with an IMU orientation source, the published fallback orientation comes entirely from the existing non-IMU HMD-relative/static fallback. IMU orientation and angular velocity are not merged into the fallback output. Position prediction, lever-arm trajectory and acceleration compensation continue to use the complete physical IMU sample internally.

Typical stream:

```text
controller_input -> runtime_controller_state
```

The resulting runtime controller state can be consumed by the OpenVR driver.

`hand_orientation_offset` is optical controller-alignment data. Its
`only_optic` field defaults to `true`, so the offset is applied only while the
effective orientation source for that side is hand tracking rather than an
active controller IMU. IMU-controlled sides should use their dedicated
`controller_override.imu_orientation.*.orientation_offset` instead.

Independently, the offset is applied per side only while a fresh
`ControllerInputV3` frame marks that side as present:

```bash
HAND_ORIENTATION_OFFSET_ONLY_RUNTIME=1
```

Set it to `0` to apply the configured offset even when
`override_controller` is absent. This does not override `only_optic`; set
`"only_optic": false` in the transform config to also apply the offset to
IMU-controlled sides.

IMU yaw correction deliberately excludes `hand_orientation_offset` and
`controller_override.imu_orientation.*.orientation_offset` from its comparison.
It compares coordinate/axis-corrected optical and IMU poses, then applies the
retained yaw correction only to the final published IMU orientation where the
configured offsets remain active. The uncorrected physical IMU orientation is
kept for position prediction, lever-arm trajectory and acceleration conversion,
so periodic and reacquire yaw correction cannot move the hand in space. This
also prevents presentation/mounting offsets from being learned again as yaw
drift.

Controller input TCP uses the same packed `ControllerInputV3` payload as SHM
(`CIV3`, protocol version 3, 1432-byte payload), including the complete per-side
IMU state and up to four IMU samples.

## Body trackers

Body tracker input can be enabled through SHM or UDP, then republished as runtime body trackers.

Example:

```bash
BODY_TRACKERS_INPUT=shm
PUBLISH_RUNTIME_BODY_TRACKERS=1
```

Optional body tracker stability gate:

```bash
RUNTIME_BODY_TRACKER_STABILITY_GATE=1
RUNTIME_BODY_TRACKER_HOLD_LOST_MS=150
RUNTIME_BODY_TRACKER_PREDICT_LOST_MS=350
RUNTIME_BODY_TRACKER_MAX_PREDICTION_VELOCITY_MPS=0.8
RUNTIME_BODY_TRACKER_PREDICTION_DAMPING=0.30
RUNTIME_BODY_TRACKER_PUBLISH_PREDICTED_VELOCITY=0
RUNTIME_BODY_TRACKER_REACQUIRE_BLEND_MS=180
```

This keeps short body tracker losses from immediately dropping the tracker. Prediction is applied per tracker, not to the whole body set. Set `RUNTIME_BODY_TRACKER_PUBLISH_PREDICTED_VELOCITY=1` only when the downstream runtime should also receive the decaying synthetic linear velocity; the default `0` avoids double prediction.

To estimate each tracker's launch velocity from a rolling real-pose history:

```bash
RUNTIME_BODY_TRACKER_PREDICTION_WINDOW_MODE=1
RUNTIME_BODY_TRACKER_PREDICTION_WINDOW_MS=500
```

Controller IMU position prediction has an independent optical history:

```bash
RUNTIME_CONTROLLER_IMU_PREDICTION_WINDOW_MODE=1
RUNTIME_CONTROLLER_IMU_PREDICTION_WINDOW_MS=500
```

An independent lever-arm trajectory mode can curve the position prediction from
live IMU orientation without changing the hold/predict/reacquire state machine:

```bash
RUNTIME_CONTROLLER_IMU_LEVER_ARM_MODE=1
RUNTIME_CONTROLLER_IMU_LEVER_ARM_LEFT_X_M=0
RUNTIME_CONTROLLER_IMU_LEVER_ARM_LEFT_Y_M=0
RUNTIME_CONTROLLER_IMU_LEVER_ARM_LEFT_Z_M=-0.12
RUNTIME_CONTROLLER_IMU_LEVER_ARM_RIGHT_X_M=0
RUNTIME_CONTROLLER_IMU_LEVER_ARM_RIGHT_Y_M=0
RUNTIME_CONTROLLER_IMU_LEVER_ARM_RIGHT_Z_M=-0.12
```

The vector is pivot-to-controller in the final controller-local frame. Window
mode and lever-arm mode are independent and may be enabled in any combination.
The lever-arm path subtracts `omega x r` from the selected launch velocity to
avoid counting the initial tangential velocity twice. Accelerometer integration
remains the existing optional additive trajectory term.

When accelerometer integration and lever-arm trajectory are both enabled, two
independent opt-in corrections can remove rotational acceleration that the
lever-arm path already represents:

```bash
RUNTIME_CONTROLLER_IMU_LEVER_ARM_CENTRIPETAL_COMPENSATION=0
RUNTIME_CONTROLLER_IMU_LEVER_ARM_TANGENTIAL_COMPENSATION=0
RUNTIME_CONTROLLER_IMU_LEVER_ARM_ANGULAR_ACCELERATION_SMOOTH_ALPHA=0.15
RUNTIME_CONTROLLER_IMU_LEVER_ARM_MAX_ANGULAR_ACCELERATION_RAD_S2=50.0
```

Centripetal compensation subtracts `omega x (omega x r)`. Tangential
compensation derives angular acceleration from gyro samples, low-pass filters
it, and subtracts `alpha x r`. The exact IMU board position inside inexpensive
controllers is not reliably known, so both corrections intentionally reuse the
configured pivot-to-controller lever-arm vector as a pivot-to-sensor
approximation. All values default to disabled and modify only the acceleration
fed into the existing `Predicting` trajectory integrator; state transitions and
timers are unchanged.

The controller mode replaces only the legacy backend instantaneous velocity
with the rolling optical estimate. Existing hold/predict timing, damping,
velocity clamp, reacquire blend, and optional accelerometer integration remain
unchanged.

The final IMU-controller predicted position also has an independent speed
limit:

```bash
RUNTIME_CONTROLLER_IMU_MAX_PREDICTION_SPEED_MPS=2.0
```

The limit constrains both the selected launch velocity and the final output
after acceleration integration and lever-arm displacement. `0` disables this
additional speed guard; the existing shared launch-velocity cap still applies.
The prediction state machine, time/path terminals, and lost-hand fallback are
unchanged.

## Spatial proxy mesh

The adapter can receive spatial proxy mesh data from `spatial_mapper` and republish it in runtime coordinates.

Typical flow:

```text
spatial_proxy_mesh
  -> xr_runtime_adapter
  -> runtime_spatial_proxy_mesh
```

This is intended for runtime debug visualization and future spatial collision/overlay experiments.

## Debugging

Inspect registered streams:

```bash
cat /tmp/tracking_streams.json
cat /tmp/runtime_tracking_streams.json
```

Enable hand gate CSV debug if supported by the current build:

```bash
RUNTIME_HAND_GATE_DEBUG_CSV=/tmp/runtime_hand_gate.csv \
runtime_adapters/xr_runtime_adapter/scripts/linux/start_xr_runtime_adapter_shm.sh

tail -f /tmp/runtime_hand_gate.csv
```

Useful things to check:

```text
stream exists
stream frame counter increases
stream age stays low
hand status does not immediately become lost
runtime_controller_state still receives hand/controller data
```

## Notes

* The adapter should own runtime coordinate correction.
* Debug viewers should usually run without applying an additional transform, otherwise coordinates may be transformed twice.
* Most experimental features are opt-in through environment variables.
* Defaults should preserve the stable runtime path unless a feature is explicitly enabled.