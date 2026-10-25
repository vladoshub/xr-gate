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

## Optional spatial and video for Monado

The application TCP server accepts `--app-tcp-spatial` to send lossless optional
`spatial_mesh` messages in addition to tracking frames. It defaults to off.
Mesh updates are cached and sent only when changed or a client connects; tracking
and mesh use alternating bounded latest-message queues. The updated shared
SteamVR/Monado TCP client recognizes both message types. Older clients need an
update before enabling these extra messages on their endpoint.

`--publish-runtime-video-tcp --runtime-video-tcp-host 127.0.0.1
--runtime-video-tcp-port 45812` forwards the selected `--video-input` as the existing
binary `XR_STEREO_VIDEO_V1` protocol on a separate optional endpoint. Its sequence
continues across video backend restarts. SHM/video output flags are independent.
See [the Monado spatial/video setup guide](../../drivers/monado_driver/docs/SPATIAL_VIDEO_TCP.ru.md).

## Application TCP / UDP access

Export `RUNTIME_APP_TCP=1` before launching to enable the read-only application
TCP server on `127.0.0.1:45810`. It streams JSON Lines containing runtime HMD,
21-joint hands, and controller poses/buttons to multiple applications. It is
**disabled by default**, uses a separate network worker, and also works without
SHM outputs. Test it with `python3 tools/xr_app_client.py` from the repository root.

See [Application network access](docs/application_network_api.md) for configuration,
the JSON protocol, existing runtime UDP outputs, binary stereo-video TCP, client
examples, and video bandwidth calculations.

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
RUNTIME_HAND_TRACKING_TIME_DECAY=1
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

An independent, default-off **emulation controller** uses thumb extension only
as a D-pad activation switch and wrist tilt for direction. In the operating pose
(middle and ring curled), index extension gives trigger and little-finger
extension gives grip; all three channels can work together. Thumb/index rings
with the other three fingers curled/extended give A/B. A V sign (index and middle
extended, ring and little curled) gives pad click regardless of thumb position. Menu requires both
open hands together; one-hand Menu/Home and finger-count shortcuts are removed.
The existing `--override-controller-block-gestures-while-stream-present` gate
also blocks emulation gestures. It defaults to true in the launcher's default
`controller_buttons_runtime_only` mode, with a 2000 ms latch after the last fresh
stream frame. Physical buttons remain available while gestures are blocked.
Set `OVERRIDE_CONTROLLER_BLOCK_GESTURES_WHILE_STREAM_PRESENT=0` to combine them;
`hand_plus_controller` continues to allow both independently of this gate.
When allowed, physical `override_controller` buttons combine with these gestures;
trigger/grip use the larger value. A moving, touched or clicked physical pad takes priority
over wrist axes and D-pad directions. Idle hardware does not block emulation,
and physical buttons keep working when hand tracking is lost. Legacy hand
gestures remain disabled on emulated sides.
Enable with `EMULATION_CONTROLLER=1` or `--emulation-controller 1`.
Use the version 3 profile. Tilt is world up expressed in the palm basis, so yaw
does not move the stick. Calibration records each cardinal separately, includes
a measured neutral deadzone and normalizes finger extension. Fold detection
distinguishes PIP/knuckle flexion from a hooked fingertip. Old wrist calibrations
must be regenerated. A/B can transition directly while gameplay stays blocked
until a curled neutral. Timed recordings are available with
`python3 tools/xr_gate_gesture_debug.py --full` from this adapter's directory.
See [setup, profiles and recording (Russian)](docs/emulation_controller.ru.md).

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

Set `RUNTIME_CONTROLLER_MODE` to override `controller_override.mode` from the transform config in the Linux start script; leaving it empty keeps the configured mode. The Windows launcher sends `XR_CONTROLLER_MODE=no` for head-only sessions, disables hand/controller services and selects `controller_only_pose_invalid` with `CONTROLLER_INPUT_TRANSPORT=none`. Gestures remains the launcher default.

The adapter can merge hand tracking with external controller input. This is useful when hand pose is available, but buttons, triggers, sticks, or fallback input should come from an external controller.

When an HMD-relative lost-hand fallback is active together with an IMU
orientation source, the published fallback orientation comes entirely from the
existing non-IMU HMD-relative/static fallback. IMU orientation and angular
velocity are not merged into the fallback output. Position prediction and
acceleration integration continue to use the physical IMU samples internally.

Typical stream:

```text
controller_input -> runtime_controller_state
```

The resulting runtime controller state can be consumed by the OpenVR driver.

Both runtime controller velocities use tracking/world axes. IMU gyro is rotated
from calibrated controller-body axes with the final presentation orientation
before publication. See [Alyx controller motion diagnostics](docs/alyx_controller_motion.md)
for the optical velocity path, long-session tests and failure capture steps.

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
kept for position prediction and acceleration conversion, so periodic and
reacquire yaw correction cannot move the hand in space. This also prevents presentation/mounting offsets from being learned again as yaw
drift.

The runtime defaults to smooth yaw correction. It compares interpolated IMU
orientation with an accepted optical pose at the camera source timestamp, then
advances only the retained yaw offset each runtime tick. Gyro motion is not
scaled or low-pass filtered. Initial alignment waits for 1000 ms of consistent,
distinct source-time pairs; later correction,
including optical reacquire, has a degrees-per-second limit. Source timestamps
are required in smooth mode; legacy timestamp-less producers can use periodic
mode instead.

```bash
export RUNTIME_CONTROLLER_IMU_YAW_CORRECTION_SMOOTH=1
export RUNTIME_CONTROLLER_IMU_YAW_SMOOTH_DEADBAND_DEG=1
export RUNTIME_CONTROLLER_IMU_YAW_SMOOTH_MAX_SPEED_DEG_S=2
export RUNTIME_CONTROLLER_IMU_YAW_SMOOTH_TAU_MS=2000
export RUNTIME_CONTROLLER_IMU_YAW_MAX_SOURCE_AGE_MS=250
export RUNTIME_CONTROLLER_IMU_YAW_REFERENCE_STABLE_MS=1000
export RUNTIME_CONTROLLER_IMU_YAW_REFERENCE_MAX_RANGE_DEG=3
```

The deadband is soft: only the residual outside it contributes to correction.
The time constant sets an exponential response; MAX_SPEED_DEG_S limits its
speed even for large errors. These values are an initial aiming-oriented
profile, not hardware-validated tuning. Raw gyro response remains unchanged.
The new options also read these environment variables directly through CLI11,
so installed launch scripts from before this change can use them without being
replaced. An unconditional `[xr_runtime_adapter][yaw-config]` startup line
identifies `algorithm=relative_rotation_v2` and its effective settings.

Only the first reference uses projected forward headings. Subsequent targets
compare the world-Y twist of full optical/IMU rotations relative to that pair.
A fixed local palm-to-sensor mounting cancels from these relative rotations.
Subtracting absolute projected headings at every frame can otherwise invent
nearly 180 degrees of yaw drift when a rigidly held wrist pitches or rolls.
A second trusted reference covers the first reference's relative 180-degree
horizontal-swing singularity. If neither reference is observable, correction
waits and preserves the retained yaw; it does not invent a new frame.

Startup confirmation checks the range of the relative yaw target, not whether
the controller stays still. Useful IMU rotation remains live while confirming.
Loss, rejected/low-confidence optics or a source-time gap over 150 ms restart
an unconfirmed reference. Cached frames cannot advance the source-time clock.
An optical outlier outside REFERENCE_MAX_RANGE_DEG restarts confirmation;
the default REFERENCE_STABLE_MS is 1000 (0 retains immediate initialization).
This presentation reference is separate from the physical acceleration frame's
four-second confirmation. `[xr_runtime_adapter][yaw-reference]` reports its
readiness independently; the client's IMU prediction calibration message refers
to the physical frame. Saved mounting and accelerometer calibration is unchanged.

Rejected/predicted/held optical poses, confidence below MIN_CONFIDENCE (default
0.5), excessive source age and unobservable relative rotations suspend correction.
A nearly vertical forward heading cannot establish the initial reference, but
does not prevent correction after an observable full-rotation reference exists.
`RUNTIME_CONTROLLER_IMU_YAW_MAX_SOURCE_AGE_MS` bounds the age of a matched
optical/IMU pair and the retained smooth target. It defaults to 250 ms, matching
the runtime's default hand age budget. A 30 Hz camera arriving 140 ms after
measurement can have usable evidence aged 140--173 ms; the former fixed 150 ms
cutoff intermittently stopped correction despite accepted optical poses.
The setting is also available as `--runtime-controller-imu-yaw-max-source-age-ms`
(1--1000 ms). This permits bounded delivery latency; it does not compare the
camera pose with the latest IMU pose. Both still match at camera source time.
Frozen observations and targets expire at the configured limit. The separate
IMU interpolation-gap and optical jump-confirmation continuity checks stay
unchanged, as do physical force-heading calibration and position prediction.
Optical yaw innovations above MAX_JUMP_DEG (default 15) must remain within
3 degrees of the candidate for JUMP_HOLD_MS (default 200) before becoming a target. These
checks compare matched yaw differences, so a real rigid turn is not a jump.
The last confirmed yaw difference survives optical loss and stale/low-quality
observations. A large change in the first returning palm pose still needs the
same jump confirmation; a gap only discards the pending candidate's support.
An AHRS epoch, device or explicit controller-state reset clears that reference.
Stale targets cannot resume correction until a fresh pair arrives; long runtime
stalls do not accumulate a catch-up correction budget. This presentation yaw
does not change the physical frame used by acceleration/position prediction.

Runtime status lines distinguish the optical observation from the retained yaw
target: `controller_*_yaw_optical_reason` reports `no_tracked_pose`,
`invalid_quaternion`, `not_accepted` (hand gate or frame mismatch),
`low_confidence`, `missing_source_time`, `waiting_imu_pair`, `future_pair`,
`pair_too_old` or `matched`. The per-side `yaw_pair_age_ms`, `yaw_target_age_ms`
and `raw_optical_confidence` fields explain why a correction is suspended even
when position tracking is available. A logged `yaw_error_deg` can be retained
from an earlier observation while optics are rejected; it is not necessarily
the current visible wrist error.
The `yaw_ready`, `yaw_reference_elapsed_ms`, `yaw_relative_condition` and
`yaw_desired_deg` fields distinguish startup confirmation, poor rotation
observability and a newly measured target from the filtered output.
`controller_publish_ns` and `controller_receive_ns`, together with the existing
`controller_tick_ns` and per-side `imu_orientation_ns`, separate sample age from
publication/transport age. Sample age alone does not identify the delayed stage.

Set `RUNTIME_CONTROLLER_IMU_YAW_CORRECTION_SMOOTH=0` to select the previous
periodic hold/interval/blend mechanism. Those periodic settings are ignored in
smooth mode. The existing CONTINUOUS and ON_REACQUIRE switches still control
whether subsequent corrections are enabled.

To keep initial optical yaw alignment but disable subsequent periodic and
optical-reacquire adjustments, export these values before launching the client:

```bash
export RUNTIME_CONTROLLER_IMU_YAW_CORRECTION_CONTINUOUS=0
export RUNTIME_CONTROLLER_IMU_YAW_CORRECTION_ON_REACQUIRE=0
```

Keep yaw correction enabled for the IMU side to retain initial alignment.
Verify that the runtime startup log reports both
`runtime_controller_imu_yaw_correction_continuous: false` and
`runtime_controller_imu_yaw_correction_on_reacquire: false`. Changing a shell
default such as `${VARIABLE:-0}` does not override an inherited value of `1`.
Device launchers also prefer the installed
`bin/scripts/runtime_adapters/xr_runtime_adapter/start_xr_runtime_adapter_shm.sh`
over the source script when the installed copy exists.

IMU dropped-batch counter changes within the same device/epoch and overflow of
the runtime's pending IMU history clear position integration and optical-anchor
history, while preserving retained yaw and any active yaw check/blend. They do
not cause another initial alignment. An AHRS epoch change, controller replacement,
transport discontinuity/reader reattachment, or backend control reset still
clears yaw because the old orientation or tracking reference may no longer be
valid. Initial alignment can run again at those boundaries even with the two
options above disabled.

State-change logs include `reason=dropped_batches`, `reason=history_overflow`,
`reason=epoch_change`, `reason=device_change`, or
`reason=transport_discontinuity`, followed by `yaw=retained` or `yaw=reset`.
Periodic status lines also expose per-side `controller_left_imu_epoch`,
`controller_left_imu_dropped`, and `controller_left_yaw_update_ns` (and their
right-side equivalents) to correlate an orientation jump with a state reset.
The existing backend control reset log reports its `reset_counter` change.

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
RUNTIME_BODY_TRACKER_TIME_DECAY=1
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

During IMU position prediction, every new timestamped accelerometer sample in
`ControllerInputV3` is integrated once in source-timestamp order. This preserves
all samples supplied by providers that batch multiple IMU readings in one frame
and prevents repeated runtime ticks from integrating the same SHM data twice.

The window mode replaces only the legacy backend instantaneous velocity
with the rolling optical estimate. Existing hold/predict timing, damping,
velocity clamp, reacquire blend, and optional accelerometer integration remain
unchanged.

The final IMU-controller predicted position also has an independent speed
limit:

```bash
RUNTIME_CONTROLLER_IMU_MAX_PREDICTION_SPEED_MPS=2.0
```

The limit constrains both the selected launch velocity and the final output
after acceleration integration. `0` disables this additional speed guard; the
existing shared launch-velocity cap still applies.
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
