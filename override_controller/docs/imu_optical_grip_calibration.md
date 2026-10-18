# Optical grip calibration (IMU controllers, Linux)

This wizard estimates the constant local rotation from a controller IMU to the
Mercury wrist/controller orientation for each selected hand independently. It fits
`optical(t) = session_world_yaw * imu(t) * grip_mount` from matched source-time
poses. A world-yaw value belongs to one AHRS session and is never saved into
the controller config. The resulting local mount replaces
`devices[].orientation_offset`; mappings and acceleration calibration stay intact.

The neutral-pose `calibrate_controller_orientation_offset.py` tool targets the
identity orientation in one pose. This tool uses repeated optical observations
across pitch, roll and yaw to align a rigid grip with Mercury.

## Runtime prerequisites

- Restart XR Gate with the same controller config supplied to `--config`. Keep the
  config unchanged throughout capture and until applying the result.
- Configure the source axes with `devices[].orientation_transform`. Any finite
  basis rotation, axis-inversion combination or disabled/identity transform is
  accepted for a new recording when the published IMU world is +Y up. The wizard
  reads diagnostics after this transform and does not apply the basis twice.
  Additional runtime IMU orientation transforms must be disabled for the selected
  side(s). Existing provider pre/post offsets are removed from observations.
- Each selected IMU must be active and have a unique config `devices[].id` entry
  with `imu_side: left/right`. Matching uses these IDs; no UUID or MAC is generated.
  Use `--side left`, `--side right` or `--side both` (the live default). An absent
  or inactive unselected controller does not block single-hand calibration.
- New recordings validate the IMU world vertical during the neutral hold using
  `imu_q`, `imu_force`, `imu_gyro`, sample flags and source timestamps from the
  existing native diagnostics. At least 10 unique stationary samples spanning
  250 ms are required; quaternion/force times must agree within 20 ms. Gyro speed
  must be <=0.35 rad/s and force magnitude within 1.5 m/s² of standard gravity.
  The estimated up direction must be within 8 degrees of +Y with at most 5 degrees
  angular spread (90th percentile). Wrong world axes, missing diagnostics or an
  uninitialized AHRS reject the round; a local mount cannot repair world tilt.
- Native optical grip/yaw support is required when applying the config:
  the provider must report `optical_grip_vectors_revision=1` and the runtime
  must have source-time yaw diagnostics (`controller_left_yaw_reference_ns`).
  These native changes are included in the companion optical-yaw patch/package.
  Adding only these Python tools to an older native build does not add that support.
- Turn on runtime diagnostic frames with `PRINT_EVERY=1` and
  `XR_HAND_POSE_DIAGNOSTICS=1`. The default input log is
  `/tmp/xr_backend_client_logs/xr_runtime_adapter.log`; `LOG_DIR` or `--log`
  can select another location.

The wizard uses provider IMU orientation and raw optical orientation, never
the displayed IMU orientation with runtime yaw correction. Ordinary yaw
correction can remain enabled during capture.

## Dependencies and source launch

From the repository root:

```bash
python3 -m venv ~/.venvs/controller-grip
~/.venvs/controller-grip/bin/python -m pip install -r override_controller/debug/calibration_requirements.txt
```

Restart XR Gate in the first terminal:

```bash
cd ~/xr-gate-release/xr-gate
PRINT_EVERY=1 XR_HAND_POSE_DIAGNOSTICS=1 ./run_xr_client.sh --config xreal_ultra
```

In a second terminal, from the repository root:

```bash
CALIBRATION_PYTHON=~/.venvs/controller-grip/bin/python \
  bash override_controller/debug/calibrate_grip.sh \
  --config ~/.config/xr_tracking/override_controller/gearvr.json \
  --runtime-root ~/xr-gate-release/xr-gate
```

For a custom controller, pass its config with `--config /path/controllers.json`.
Append `--side right` to calibrate only the right controller. Each side has its
own basis, offset and device snapshot.

The same command is available from the installed release tools:

```bash
CALIBRATION_PYTHON=~/.venvs/controller-grip/bin/python \
  bash ~/xr-gate-release/xr-gate/bin/override_controller/tools/calibrate_grip.sh
```

## Capture and stability

The terminal gives Russian instructions for the selected hand(s), right before
left when both are selected. Keep your head still, hold the controller as in a game, keep the current
hand in view, and move the other hand out of view. Each round records a neutral
hold, forward/backward pitch, axial wrist roll and left/right yaw. Make three
slow repetitions per movement. Each phase has 2 seconds to prepare and 8 to
record. Pure yaw or a single static pose cannot satisfy the required motion diversity.

The default stopping criteria are:

| Criterion | Default |
| --- | --- |
| Consecutive usable rounds per side | 3 |
| Maximum rounds per side | 6 |
| Maximum mount spread between rounds | 3 degrees |
| Verification yaw error, 90th percentile | 12 degrees |
| Verification full-orientation median error | 25 degrees |

IMU orientations are interpolated at camera source time. Cached ticks add no
weight, samples older than 150 ms are discarded, and interpolation cannot span
an IMU gap over 80 ms. Held/rejected optical poses, IMU/HMD epoch resets,
insufficient multi-axis excitation and excessive head motion reject a round.
The mount is fitted on preceding rounds and checked as a fixed mount on the
newest round. That round also participates in the stopping decision, so the
reported metrics are diagnostic, not an independent physical accuracy estimate.
Stability is measured against Mercury; an optical bias can still be consistent.

The output goes into a new directory under
`~/.local/share/xr_tracking/override_controller/optical_grip/` by default, away
from the source tree. Use `--out` to select a new directory explicitly.
`recording.jsonl` and reports are saved even if calibration fails. A candidate
is written only when all selected sides are stable. Each mount is fitted separately.
Ctrl+C stops capture without changing the config.

## Apply and restore

On success the wizard prints the exact application command. Stop XR Gate
before executing it. The command uses `apply_optical_grip_calibration.py` with
`--replace-existing --write`. It checks native support, the recorded config hash
and previous offsets, then atomically replaces only the selected device offsets.
New candidates also compare the recorded device fingerprint and exact source
`orientation_transform` with the active config, even on an idempotent reapply.
The config's source axes, other controller, button bindings, accelerometer
calibration and session yaw are not modified.
A timestamped `gearvr.json.before-guided-grip-*.bak` backup is created.
Restart XR Gate afterwards and test aiming and larger rotations.

To restore the previous grip, stop XR Gate, copy that backup over the same
config path, and restart. Reapplying an identical candidate is idempotent.

## Updating these tools without rebuilding

This generalization changes Python/shell tools and their packaging lists, with
no C++ or stream-contract changes. Existing native builds with the prerequisites
above can keep their binaries. From the patched source checkout:

```bash
TOOLS_DST="$HOME/xr-gate-release/xr-gate/bin/override_controller/tools"
install -d "$TOOLS_DST"
install -m 755 override_controller/debug/{calibrate_optical_grip.py,apply_optical_grip_calibration.py,optical_grip_config.py,calibrate_grip.sh} "$TOOLS_DST/"
install -m 644 override_controller/docs/imu_optical_grip_calibration.md "$TOOLS_DST/"
```

The new `optical_grip_config.py` must be next to both Python entry points. When
running from `override_controller/debug`, no copy or rebuild is needed. Existing
saved Gear VR calibration values remain usable without recalibration. Applying
a newly measured mount still requires stopping and restarting XR Gate.

## Recording compatibility

New recordings/reports/candidates use the generic
`xr_controller_guided_grip_recording_v2`, `xr_controller_guided_grip_report_v2`
and `xr_controller_optical_grip_trial_v2` schemas. The header records the selected
sides, full source-basis/offset snapshots and device fingerprints.

Old `gearvr_*_v1` recordings and candidates remain accepted under their original
X=-90/no-inversions restriction. Replay preserves their legacy schema and
explicitly reports that measured gravity validation is unavailable. This does
not impose the old basis restriction on new recordings. Replay defaults to the
recorded side selection; `--side` can select a subset.

## Replay and tests

```bash
CALIBRATION_PYTHON=~/.venvs/controller-grip/bin/python \
  bash override_controller/debug/calibrate_grip.sh \
  --replay /path/to/recording.jsonl --out /path/to/new-result

~/.venvs/controller-grip/bin/python override_controller/tests/calibrate_optical_grip_test.py -v
```

With `OVERRIDE_CONTROLLER_BUILD_TESTS=ON`, CMake registers
`calibrate_optical_grip_test` when its configured Python can import NumPy/SciPy.
Set `Python3_EXECUTABLE` to the virtual environment interpreter when configuring
to ensure this test is included. CMake installs the wizard, its application tool,
launcher and dependency list together in `bin/override_controller/tools`;
the Linux component installer copies the same files.
