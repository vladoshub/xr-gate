# Optical grip calibration (Gear VR, Linux)

This wizard estimates the constant local rotation from a Gear VR IMU to the
Mercury wrist/controller orientation for each hand independently. It fits
`optical(t) = session_world_yaw * imu(t) * grip_mount` from matched source-time
poses. A world-yaw value belongs to one AHRS session and is never saved into
the controller config. The resulting local mount replaces
`devices[].orientation_offset`; mappings and acceleration calibration stay intact.

The neutral-pose `calibrate_controller_orientation_offset.py` tool targets the
identity orientation in one pose. This tool uses repeated optical observations
across pitch, roll and yaw to align a rigid grip with Mercury.

## Runtime prerequisites

- Restart XR Gate with the same `gearvr.json` supplied to the wizard. Keep the
  config unchanged throughout capture and until applying the result.
- Use the standard provider `orientation_transform`: enabled, X=-90 degrees,
  Y=Z=0, no axis inversions. Additional runtime IMU orientation transforms must
  be disabled. Existing provider pre/post offsets are removed from observations.
- Both IMUs must be active and have unique config `devices[].id` entries with
  `imu_side: left/right`. Matching uses these IDs; no UUID or MAC is generated.
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
python3 -m venv ~/.venvs/gearvr-grip
~/.venvs/gearvr-grip/bin/python -m pip install -r override_controller/debug/calibration_requirements.txt
```

Restart XR Gate in the first terminal:

```bash
cd ~/xr-gate-release/xr-gate
PRINT_EVERY=1 XR_HAND_POSE_DIAGNOSTICS=1 ./run_xr_client.sh --config xreal_ultra
```

In a second terminal, from the repository root:

```bash
CALIBRATION_PYTHON=~/.venvs/gearvr-grip/bin/python \
  bash override_controller/debug/calibrate_grip.sh \
  --config ~/.config/xr_tracking/override_controller/gearvr.json \
  --runtime-root ~/xr-gate-release/xr-gate
```

After a native build/install, the same command is available in a release:

```bash
CALIBRATION_PYTHON=~/.venvs/gearvr-grip/bin/python \
  bash ~/xr-gate-release/xr-gate/bin/override_controller/tools/calibrate_grip.sh
```

## Capture and stability

The terminal gives Russian instructions, first for the right hand and then the
left. Keep your head still, hold the controller as in a game, keep the current
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
is written only when both sides are stable. The left mount is fitted separately.
Ctrl+C stops capture without changing the config.

## Apply and restore

On success the wizard prints the exact application command. Stop XR Gate
before executing it. The command uses `apply_optical_grip_calibration.py` with
`--replace-existing --write`. It checks native support, the recorded config hash
and previous offsets, then atomically replaces only the selected device offsets.
A timestamped `gearvr.json.before-guided-grip-*.bak` backup is created.
Restart XR Gate afterwards and test aiming and larger rotations.

To restore the previous grip, stop XR Gate, copy that backup over the same
config path, and restart. Reapplying an identical candidate is idempotent.

## Replay and tests

```bash
CALIBRATION_PYTHON=~/.venvs/gearvr-grip/bin/python \
  bash override_controller/debug/calibrate_grip.sh \
  --replay /path/to/recording.jsonl --out /path/to/new-result

~/.venvs/gearvr-grip/bin/python override_controller/tests/calibrate_optical_grip_test.py -v
```

With `OVERRIDE_CONTROLLER_BUILD_TESTS=ON`, CMake registers
`calibrate_optical_grip_test` when its configured Python can import NumPy/SciPy.
Set `Python3_EXECUTABLE` to the virtual environment interpreter when configuring
to ensure this test is included. CMake installs the wizard, its application tool,
launcher and dependency list together in `bin/override_controller/tools`;
the Linux component installer copies the same files.
