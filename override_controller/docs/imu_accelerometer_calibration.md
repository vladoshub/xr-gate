# Accelerometer calibration in the controller config

The selected controller profile (`--config` / `CONFIG_PATH`) is the only source
of persistent accelerometer corrections. The old separate
`controller_imu_calibration.json` and `OVERRIDE_CONTROLLER_IMU_CALIBRATION_FILE`
are not read. Legacy side files are still excluded from profile selection.

Each record refers to `devices[].id`, using the same integer `device_id` as
button bindings. No BLE UUID/address or side name is stored in a calibration:

```json
"imu_calibrations": [
  {
    "device_id": 1,
    "enabled": true,
    "accelerometer_bias_m_s2": [-0.10727315, -0.07759059, 0.43145744],
    "accelerometer_scale": [0.99019083, 0.99454659, 0.98612358]
  }
]
```

`override_controller/configs/empty_gearvr.json` includes the previously measured
coefficients for configured device 1 (left) and device 2 (right), with generic
empty device fingerprints. Use `--connect-devices` to associate a template's
configured IDs with actual controllers. Calibration does not replace that
normal connection step. If a trained profile is already in use, copy only the
`imu_calibrations` array into that profile; keep its devices and bindings.

## Application and validation

Corrections operate on **native sensor axes**, in SI units, before gravity
initialization, gyro bias acquisition and Madgwick integration:

```text
corrected[i] = (raw[i] - accelerometer_bias_m_s2[i]) * accelerometer_scale[i]
```

Per-device `orientation_transform` is applied later, before publication.
`orientation_offset` changes only the published quaternion. Correction does
not normalize force or remove the real acceleration used for prediction.

The core uniquely resolves the configured device and verifies that the actual
provider exposes both gyroscope and accelerometer capabilities. Button-only
providers receive no calibration. Ambiguous matches receive no correction;
two enabled calibrated IDs resolving to one IMU are rejected. Missing/disabled
entries use the uncalibrated path. Unknown/duplicate IDs, malformed vectors,
nonfinite values, bias outside ±2 m/s² and scale outside 0.5..2 are rejected
when loading a profile.

Gear VR BLE and XR Controller serial providers apply these values once to each
fresh native sample. Changing correction clears old queued measurements and
reinitializes AHRS/gyro acquisition. Repeated synchronization with identical
values keeps the filter and queue. Reconnects retain the selected correction.
Samples and state set `CONTROLLER_IMU_ACCELEROMETER_CALIBRATED` (64) when applied.
The startup/first-live-packet message reports `device_id`, `source=config` and
the values; it is not emitted for every frame.

The coefficients follow the configured integer ID after `--connect-devices`.
When that ID is assigned to another physical controller, measure its correction
again. Device fingerprints may still be used for ordinary connection routing;
they are not calibration keys.

## Obtain a calibration on Linux

`debug/calibrate_controller_accelerometer.py` uses Python 3 standard-library
modules and `debug/record_controller_imu.py`, installed together under
`bin/override_controller/tools/`. Cameras, `tracking.csv`, and optical poses
are not required. Use the same connected profile as the running publisher.

1. In `imu_calibrations`, set `enabled: false` for the device being measured
   (or remove its entry). Leave the other controller's correction alone.
2. Restart `override_controller` with that profile. Live raw IMU samples must
   be available in the Linux `controller_input` SHM stream.
3. Run from the repository root, replacing the profile path as needed:

```bash
python3 override_controller/debug/calibrate_controller_accelerometer.py \
  --config ~/.config/xr_tracking/override_controller/gearvr.json \
  --device-id 1 --output ./gearvr-device-1-capture.json --write-config
```

4. Follow six prompts: `+X`, `-X`, `+Y`, `-Y`, `+Z`, `-Z`. The tool displays
   **native** XYZ readings; rotate until the requested axis is approximately
   +9.8 or -9.8 and the others are near zero. Hold still, preferably supported
   on a surface, for 3 seconds per pose. Small placement tilts are accepted.
5. The tool fits a diagonal gravity ellipsoid from all stationary samples,
   validates spread/residuals and runtime limits, and writes only the selected
   `imu_calibrations` record. It preserves other profile fields and IDs, creates
   a timestamped `.bak`, and refuses to overwrite a concurrently changed profile.
6. Restart `override_controller` to load the new record. Repeat with
   `--device-id 2` after disabling/restarting for that device.

`--write-config` is optional: without it, the script only prints the result and
saves the requested report. Reports contain integer IDs and native-axis samples,
not Bluetooth identities. Keep them outside the controller profile directory.
Refit a saved capture without another hardware session:

```bash
python3 override_controller/debug/calibrate_controller_accelerometer.py \
  --config ~/.config/xr_tracking/override_controller/gearvr.json \
  --device-id 1 --samples ./gearvr-device-1-capture.json --write-config
```

Use `--registry` / `--stream` for custom SHM settings. If backend
`gravity_magnitude` differs from 9.80665, pass that value via `--gravity`.
Captures that still carry calibrated flags are rejected instead of applying a
second correction. Movement, sample gaps, epoch changes, duplicate timestamps
and missing poses cannot count toward a stationary hold.

This fixed sensor calibration is separate from runtime optical/IMU world-yaw
alignment and its stability wait. That alignment still happens during normal
tracking.

## Verification

With `OVERRIDE_CONTROLLER_BUILD_TESTS=ON`, the calibration/provider tests cover
ID routing independently of side/session order, roundtrip serialization,
capability guards, invalid/ambiguous IDs, applying correction before AHRS,
queued-data invalidation, duplicate application and reconnects. When Python 3
is available, CTest also runs the fit/capture/config-write tests. Run directly:

```bash
python3 override_controller/tests/calibrate_controller_accelerometer_test.py
```
