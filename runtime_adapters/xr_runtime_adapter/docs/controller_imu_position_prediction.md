# Controller position prediction during optical loss

The runtime anchors each controller to a confirmed optical position. During
loss it holds the position, predicts for the configured interval, then
invalidates the pose or uses the configured HMD-relative fallback. The launch
script defaults remain 30 ms hold, 600 ms prediction and 180 ms reacquire blend.
Existing speed, path and HMD distance limits still apply.

## Frames and sample time

Optical positions and velocities are transformed into runtime space together.
For camera-local streams, velocity includes the HMD rotation and available
HMD linear/angular velocity: `v_world = R * v_camera + v_hmd + omega_hmd x r`.

The provider's AHRS reference need not share XR world yaw. Physical acceleration
therefore has a separate provider-to-XR yaw alignment, learned from confirmed,
clean optical orientations. It stays fixed while optical tracking is absent;
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
Yaw is still identified independently from optical/IMU rotations. An explicit
`madgwick_beta=0` retains the existing gyro-only reference and startup behavior.

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
