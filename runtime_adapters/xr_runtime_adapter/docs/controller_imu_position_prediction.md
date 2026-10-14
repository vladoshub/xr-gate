# Controller position prediction during optical loss

The runtime anchors each controller to a confirmed optical position. During
loss it holds the position, predicts for the configured interval, then
invalidates the pose or uses the configured HMD-relative fallback. The launch
script defaults remain 30 ms hold, 600 ms prediction and 180 ms reacquire blend.
Existing speed, path and HMD distance limits still apply.

## Optical-to-inertial handoff

Loss prediction retains the confirmed launch velocity and integrates only
previously unconsumed acceleration after its optical measurement boundary:
`v = v_launch + integral(a dt)`. Acceleration is a signed vector; real braking
reduces velocity. Existing time decay remains optional; set
`RUNTIME_CONTROLLER_IMU_TIME_DECAY=0` to keep full response until timeout.

A packet arriving after loss is not necessarily measured after loss. The
integrator now excludes force intervals ending at or before the last accepted
optical source timestamp and clips straddling intervals at that boundary.
Otherwise delayed pre-anchor force changes a launch velocity already inferred
from the visible trajectory. If launch alignment has advanced velocity to a
newer IMU sample, that integration cursor remains the lower bound as well.

The optical measurement timestamp is independent of the runtime loss/hold
clock. Valid acceleration during an intentional hold still changes momentum,
while the displayed position remains held. Optical acceptance/reacquisition
clears the previous loss deltas; duplicate sensor timestamps cannot apply them
twice. A prediction reset clears the measurement boundary with the anchor.

This corrects delayed force crossing the handoff boundary. It does not certify
accelerometer bias/gravity compensation or guarantee accurate long optical
losses. Force-heading calibration and IMU aiming are unchanged. The existing
per-side `RUNTIME_CONTROLLER_*_IMU_ACCELERATION_INTEGRATION=0` switches still
select optical-velocity prediction without force integration.

## Frames and sample time

Optical positions and velocities are transformed into runtime space together.
For camera-local streams, velocity includes the HMD rotation and available
HMD linear/angular velocity: `v_world = R * v_camera + v_hmd + omega_hmd x r`.

The provider's AHRS reference need not share XR world yaw. Physical acceleration
therefore has a separate provider-to-XR yaw alignment, learned from confirmed,
clean optical/IMU motion. It stays fixed while optical tracking is absent;
presentation yaw blends cannot rotate an active position trajectory. Packet
loss preserves the reference, while an AHRS epoch/device change resets it.
Until alignment is available, optical velocity can predict position but force
in an unknown frame is not integrated.
Each alignment update requires a corresponding rotation pair newer than the
evidence used by the previous update. Newly rejected optical rotations cannot
repeatedly reapply a coherent estimate made from old pairs still in the window.

The shared controller Madgwick processor initializes pitch/roll from the first
credible stationary gravity observation after startup or reset. Providers keep
`CONTROLLER_IMU_ORIENTATION_VALID` clear until then, rather than advertising an
arbitrary identity orientation as a usable gravity reference. Quiet means gyro
below 0.20 rad/s and force magnitude within 1 m/s2 of configured gravity.
Yaw is still identified independently from optical/IMU motion. An explicit
`madgwick_beta=0` retains the existing gyro-only reference and startup behavior.

## Translation heading calibration

The CLI and launch script enable
`RUNTIME_CONTROLLER_IMU_TRANSLATION_YAW_REFINEMENT=1`. It also requires
`RUNTIME_CONTROLLER_IMU_TRANSLATION_YAW_CHECK=1` and acceleration integration
on the selected side. Each hand learns its own physical force heading in the
current AHRS/XR reference; no measured session angle is written to a device
`orientation_offset` or accelerometer bias/scale calibration.

The existing rotation/grip startup confirmation remains available with the
configured stability wait (four seconds by default), even if the optional
translation estimator has no usable motion. Rotation and translation have
independent confirmation clocks; rejected translation windows cannot block a
stable rotation-derived frame. A coherent translation heading can establish
the initial force frame independently when optical grip rotation is noisy.

`[imu-alignment] controller_*_acceleration_yaw_valid=yes` means either startup
estimator confirmed the force frame. The additional
`controller_*_acceleration_translation_yaw_calibrated=yes` field reports accepted
translation-heading calibration/refinement; it may stay `no` after successful
rotation-only startup. Both kinds of transition are forwarded to the client
console, independently of verbose frame logging. No forward/back translation
ritual is required before the existing alignment can finish.

Visible forward/back movements with acceleration and stops provide useful
independent heading evidence. Avoid rapid wrist rotations. A stationary hand,
uniform translation or constant acceleration alone cannot establish a translation
heading. IMU aiming/orientation remains available while initial force alignment
is pending; only force integration waits. The existing bounded optical-velocity
predictor can still bridge a loss before either calibration finishes.

The heading estimator matches optical XZ positions to double-integrated force
using source timestamps and covered delayed delivery. A 1.25-second fit removes
unknown initial position/velocity and a quadratic constant-acceleration nuisance
from both tracks. This nuisance removal is local to the estimator; measured
acceleration and actual braking remain in the position integrator. A heading
needs observable residual motion, compatible amplitude, coherence >= 0.97,
and at least five distinct agreeing endpoints over 150 ms within an eight-degree
range. Rapid rotation, unobserved force gaps and unsupported/low-quality fits
cannot establish a new heading. The 350-ms hemisphere check remains for legacy
rotation-only operation.

After startup, only new accepted heading evidence no older than 150 ms can
refine the force frame. A confirmed translation heading is applied completely
while the corresponding optical pose is accepted. A two-degree step per rare
accepted fit can leave a large residual frame error despite calibrated=yes;
the confirmed translation fit already passed the independent coherence,
observability and distinct-endpoint checks. Applying this force frame does not
change the presentation/aiming quaternion or rotate an active loss trajectory.
Retained launch-force history is re-expressed in the newly confirmed frame.
Rotation-based fits may
make small updates within the confirmed heading range, but cannot restore a
different intermediate force yaw or override it with a stale half-turn hint.
Optical loss retains the last confirmed frame; ordinary delivery/prediction
resets retain it and its consumed-evidence guard. A provider epoch/device or
backend reference reset clears it and requires calibration again.

Frame diagnostics include `controller_*_acceleration_translation_heading_deg`,
`*_heading_ns`, `*_heading_coherence` and `*_yaw_refinements` for this estimator,
separately from the older hemisphere-check fields. `orientation_offset`, rendered
controller yaw correction, jitter filtering and prediction timeout/limits are
independent of this calibration. Setting
`RUNTIME_CONTROLLER_IMU_TRANSLATION_YAW_REFINEMENT=0` disables full-heading
refinement, retaining rotation-based startup with the legacy hemisphere check.

V3 still carries one AHRS quaternion per batch. Older sample orientations are
approximately reconstructed backward with the corrected gyroscope and sample
timestamps before axis/mounting transforms. This does not reproduce the
AHRS accelerometer correction within that interval. Intervals with missing or
invalid gyro, or gaps greater than 50 ms, are not reconstructed; older forces
without a matching orientation are skipped. No wire ABI or firmware change
is required.

## Loss and reacquire

A stale SHM/UDP hand frame becomes an explicit loss observation before the
hand gate. Held frames use degraded status and zero velocity, so they cannot
refresh the real optical anchor or calibrate yaw. Loss observations advance
the gate clock even when the backend sequence stopped advancing.

Optical reacquire clears the preceding prediction's acceleration integrals
and freeze reason before blending. If tracking disappears mid-blend, prediction
starts from the last published blended position with the velocity measured
from accepted optical samples. Both normal anchors and blend targets respect
the configured optical velocity window. The velocity history uses the matching
source-time optical frame before output smoothing, only while the hand gate
accepts a real optical pose. An older optical/yaw reference cannot seed it.
With a positive `max_prediction_speed_mps`, the returning optical correction
also obeys that speed limit. A distant target may take longer than
`reacquire_blend_ms`; reaching the nominal deadline cannot snap to the target.
The moving correction publishes its actual step velocity for output consumers,
but this synthetic velocity does not become the next prediction's launch
velocity. A short optical return followed by another loss must not extrapolate
the correction toward an uncertain target for a new prediction window.
With insufficient fresh optical samples, launch velocity is zero; fresh IMU
acceleration remains available. A zero/negative speed limit retains the previous
unrestricted blending behavior.

When the optical velocity window and acceleration integration are both enabled,
the window slope is advanced to the newest consumed IMU sample before becoming
the physical launch velocity. A fitted slope belongs to historical camera
observations; using it unchanged with later IMU braking discards acceleration
between the fit and the IMU cursor and can leave a false reverse velocity.
The correction integrates the exact least-squares time-weight kernel over the
observed acceleration intervals, including irregular camera timestamps. It
does not integrate that history into the displayed optical position or again
after loss. The subsequent loss integrator consumes only newer measurements.
It uses a fixed 512-observation history per side; incomplete coverage, a gap
above 50 ms, invalid world alignment or an IMU reset prevents the correction.
Disabled acceleration integration retains the existing optical slope behavior.
Diagnostics expose `controller_*_imu_anchor_alignment_dv` and
`controller_*_imu_anchor_alignment_samples` at the existing print cadence.

During hold, the published position and velocity remain frozen. Timed IMU
measurements still update the internal velocity, which is retained when the
prediction phase starts. A batch spanning the hold/prediction boundary is split
at that boundary: pre-boundary acceleration changes velocity, while only the
post-boundary interval changes the published coordinate.

At `max_distance_m`, the published coordinate is held but internal physical
velocity is retained. Braking an outward-moving hand must not become an inward
movement merely because the output hit its limit. The position origin is
rebased to the held output, separately from the optical-loss deadline and
time-decay clock. Fresh acceleration must produce a physical candidate inside
the distance limit before recovery. The final output still obeys the speed
limit; held time cannot accumulate a larger step budget. An optical anchor
already outside the radius can return gradually after the candidate is inside.

## Time decay

With time decay enabled, `w(t) = 1 - t / T` during the prediction interval.
Published velocity is `w(t) * (v_anchor * damping + integral(a dt))`.
Position integrates that same velocity using sample-time weights, including
historical batches delivered between runtime ticks. Without new acceleration
samples, the accumulated IMU velocity propagates to the runtime tick without
integrating a duplicate sample again. Disabling decay keeps full response.

This remains a short-loss predictor. Accelerometer bias and attitude error
can cause drift; the configured timeout and limits do not provide long-term
position tracking outside the camera field of view.

Fixed per-device accelerometer bias/scale can be applied in the shared provider
processor before AHRS and force publication. See
[`imu_accelerometer_calibration.md`](../../../override_controller/docs/imu_accelerometer_calibration.md).
This retains real acceleration and works with acceleration integration enabled
for both hands.

## Verification

Enable `XR_RUNTIME_ADAPTER_BUILD_TESTS` when configuring the runtime adapter.
`controller_imu_prediction_test` covers world yaw, presentation continuity,
timed batched rotation, missing gyro, mounting offset, stale/held optics,
camera/HMD velocity, interrupted reacquire, decay/burst delivery, duplicate
samples and speed/distance/timeout limits. The two delivery tests cover the
Gear VR and serial xr_controller paths and their timestamp/queue behavior.
Distance tests also cover outward braking with both hands and decay modes,
and speed-limited recovery after a long hold at an out-of-range anchor.

`hand_optical_velocity_test` exercises the real Mercury loader and runtime gate
with a test pose-producing C ABI library. It verifies source-time derivatives,
rejected pose/quaternion outliers, duplicate/out-of-order frames, clock epochs,
loss/reacquire, rolling-window noise reduction, velocity limits and both hands.
This does not exercise neural inference or replace hardware tracking checks.

## Hardware diagnostics

XR Client's main console shows `controller_left_acceleration_yaw_valid` and
`controller_right_acceleration_yaw_valid` at runtime startup and whenever either
value changes. These short `[xr_runtime_adapter][imu-alignment]` messages are
flushed immediately and do not depend on `PRINT_EVERY`. `yes` means the physical
provider-to-world yaw reference is available; it does not certify the optical
launch velocity or guarantee drift-free position prediction. `no` means force
in that unknown frame cannot be integrated. Initial `no` is expected before
optical/IMU rotations identify the reference. The client follows only these
transitions from the existing service log, with bounded read buffers and the
existing service-stop event. Verbose frame diagnostics stay in the log file.

For a complete diagnostic run, set `log_max_bytes` to `0` in the active XR
client JSON profile and use `PRINT_EVERY=1`. The normal log limiter keeps only
the tail; stopping several seconds after a failure can otherwise remove the
loss/reacquire transition. Set the previous log limit back after the test.

Per-side runtime diagnostics include the exact published position and velocity,
pose source (`optical`, `imu_hold`, `imu_prediction`, `imu_reacquire`,
`hmd_relative`, `invalid`), raw/accepted optical status and coordinates,
IMU loss age, acceleration position/velocity deltas, HMD-relative blend mode
(0 none, 1 enter, 2 exit) and button/stick input. They distinguish an IMU
trajectory from optical reacquire and from the configured fallback after the
short prediction window expires.

Diagnostics also include the provider-frame IMU quaternion, latest timed force
and gyro with validity flags, clean optical quaternion, and HMD quaternion
(all quaternions are xyzw). These separate a force/attitude/alignment error from
a wrong optical pose; the latest raw sample logged at a runtime tick is not a
full recording of all IMU batches. Gear VR startup prints effective Madgwick
beta and logs successful gravity initialization after each AHRS reset.

Shutdown prints each controller predictor's effective settings. The separate
`hand_stability_gate_profile: IMU` summary describes only the hand validator;
its zero blend/window/distance settings do not describe the controller predictor.
