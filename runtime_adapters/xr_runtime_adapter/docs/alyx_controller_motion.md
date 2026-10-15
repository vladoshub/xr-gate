# Diagnosing controller motion in Half-Life: Alyx

If gravity-glove pulling stops working after several minutes while buttons still
work, capture motion data before restarting services. A successful restart of
`override_controller` does not isolate the fault to that process: its SHM/TCP
transport discontinuity also resets the runtime adapter's controller synthesis
state, including yaw alignment, optical position history and IMU prediction.

## Velocity path

Mercury's runtime loader derives linear velocity from consecutive camera-source
timestamps and controller positions. The runtime hand gate recomputes velocity
from accepted observations so rejected detections do not contaminate motion.
Fresh optical controller poses retain that linear velocity in
`runtime_controller_state`; the OpenVR driver copies it to `vecVelocity`.
Held/lost poses may deliberately have zero velocity, and predicted/reacquiring
poses use their configured motion policy.

IMU angular velocity takes a separate path. The provider publishes gyro in its
calibrated controller-body axes. Controller synthesis must rotate it into
tracking/world axes with the final controller orientation, including basis,
grip mounting and retained optical yaw. The OpenVR driver then copies that
world vector to `vecAngularVelocity`. Passing the body vector directly produces
a wrong prediction axis whenever the controller orientation differs from the
tracking axes. For example, at +90 degrees of yaw a local X-axis rotation is
world -Z, rather than world X.

The [OpenVR pose contract](https://github.com/ValveSoftware/openvr/blob/master/docs/Driver_API_Documentation.md#poses)
requires tracking/world-space velocities and uses them for pose prediction.
The angular-velocity correction is a confirmed contract fix. Without a capture
of an actual Alyx failure, it is not proof of a particular 3–5 minute trigger.

## Validation

The runtime tests cover both hands, native and runtime grip offsets, coordinate
basis conversion, and six simulated minutes of gradual yaw drift. A separate
six-minute test feeds repeated motions through the real Mercury loader with a
mock inference library, the hand gate, and controller synthesis. It also resets
controller synthesis halfway through the run, verifying that optical velocity
does not require a controller restart to keep working.

Build and run from the repository root, with CLI11 and nlohmann-json installed:

```sh
cmake -S runtime_adapters/xr_runtime_adapter -B build/alyx-runtime \
  -DXR_RUNTIME_ADAPTER_BUILD_TESTS=ON -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build/alyx-runtime -j2
ctest --test-dir build/alyx-runtime --output-on-failure
```

Use the rebuilt `xr_runtime_adapter` in the running installation. Rebuilding
only `override_controller` does not install this correction.

## Capture a real failure

Before starting the usual XR Gate launcher, enable diagnostics in the same shell:

```sh
export XR_HAND_POSE_DIAGNOSTICS=1
export XR_MERCURY_POSE_DIAGNOSTICS=1
export PRINT_EVERY=3
```

`PRINT_EVERY=3` samples the runtime status log at about 30 Hz with a 90 Hz runtime.
Keep the backend, runtime and override-controller logs from that launch. In a
second terminal, from the repository root, record incoming controller samples:

```sh
python3 override_controller/debug/record_controller_imu.py \
  --output alyx-controller-imu.csv --duration-sec 480
```

Record a few working pulls at the start, reproduce the failure, and make several
similar attempts after failure. Note elapsed times, which hand fails, whether
target highlighting still appears, and whether ordinary pickup/release and
shooting still work. Capture the failure before restarting a service. A second
capture after restarting only `override_controller` provides a comparison.

Inspect these per-side runtime fields:

| Fields | What they distinguish |
| --- | --- |
| `raw_optical_v`, `raw_optical_flags` | Backend derivative and its validity |
| `accepted_optical_v`, `accepted_optical_flags`, `accepted_optical_status` | Motion after the hand gate, including held or degraded poses |
| `velocity`, `angular_velocity`, `pose_source`, `pose_flags` | Motion actually submitted to the OpenVR controller stream |
| `imu_q`, `orientation`, `imu_gyro`, yaw-correction status | Provider drift, retained yaw correction and final pose |
| `imu_sample_ns`, `imu_orientation_ns`, `controller_tick_ns`, IMU drop/epoch fields | Delay, stale samples and stream resets |
| `buttons`, `trigger`, `grip` | Physical input state at the same time |

Hand velocity flags are `HAND_LINEAR_VELOCITY_VALID = 2` and
`HAND_ANGULAR_VELOCITY_VALID = 4`. Interpret zero velocity together with its
flags and tracking status; zero alone can be a valid stationary measurement.

If previously exported `RUNTIME_CONTROLLER_IMU_YAW_CORRECTION_CONTINUOUS=0`
and `RUNTIME_CONTROLLER_IMU_YAW_CORRECTION_ON_REACQUIRE=0` are still in the
launcher environment, the initial yaw reference remains but later optical drift
correction is disabled. Check the startup log for the effective values. For a
comparison run with ongoing smooth correction explicitly enabled:

```sh
export RUNTIME_CONTROLLER_IMU_YAW_CORRECTION=1
export RUNTIME_CONTROLLER_IMU_YAW_CORRECTION_SMOOTH=1
export RUNTIME_CONTROLLER_IMU_YAW_CORRECTION_CONTINUOUS=1
export RUNTIME_CONTROLLER_IMU_YAW_CORRECTION_ON_REACQUIRE=1
```

Change one setting group per comparison. The tests validate the data contract
and synthetic pipeline; actual controller drift, radio latency, optical
visibility and Alyx behavior require the hardware capture.
