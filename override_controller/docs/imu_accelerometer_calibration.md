# Per-device accelerometer calibration

Position prediction needs specific force in SI units. A stationary controller
with a force magnitude of 10.3 m/s2 instead of 9.80665 m/s2 creates roughly
0.5 m/s2 of false acceleration after gravity subtraction. Integrating that
error for 1.2 seconds can move the position by tens of centimetres.

Use six stationary orientations spanning positive and negative X, Y and Z
to fit a fixed bias and scale for each physical sensor. The correction is
`corrected_force[axis] = (measured_force[axis] - bias[axis]) * scale[axis]`.
It runs before AHRS gravity initialization, Madgwick updates and publication
of timed acceleration samples. It does not normalize individual measurements
to gravity; real linear acceleration must remain available.

## File and physical identity

The default file is
`$XDG_CONFIG_HOME/xr_tracking/override_controller/controller_imu_calibration.json`,
or `~/.config/xr_tracking/override_controller/controller_imu_calibration.json`.
The launcher uses `CONFIG_DIR/controller_imu_calibration.json` and passes its
absolute path through sudo. Override the path with
`OVERRIDE_CONTROLLER_IMU_CALIBRATION_FILE`; set it to `none` to disable loading.
Restart the provider after changing the file.
The canonical filename is excluded from controller bindings/profile discovery.
Keep a custom calibration filename outside the bindings directory.

```json
{
  "schema_version": 1,
  "devices": {
    "gearvr_ble:AA:BB:CC:DD:EE:FF": {
      "accelerometer_bias_m_s2": [0.0, 0.0, 0.0],
      "accelerometer_scale": [1.0, 1.0, 1.0]
    }
  }
}
```

Keys are provider stable device IDs, not left/right roles or the hexadecimal
device hashes in the recorder CSV. Gear VR IDs include the Bluetooth address.
Changing hand bindings cannot move one sensor's calibration onto another.
The serial xr_controller provider uses the same optional file and correction.

Values use **native sensor axes before provider basis/mounting transforms**.
The recorder CSV already contains the configured provider basis. For the usual
Gear VR basis rotation of -90 degrees about X, recorded vectors are
`(native_x, native_z, -native_y)`. Convert fitted recorded-axis bias back to
`(bias_x, -bias_z, bias_y)` and scale back to `(scale_x, scale_z, scale_y)`.
For another transform, invert that transform before fitting native values.

A missing file or unmatched device retains the existing uncalibrated behavior.
Malformed entries, non-finite values, bias magnitude above 2 m/s2 on any axis,
or scale outside [0.5, 2] are rejected. Loading/rejection logs occur when a
physical session is created, not for every measurement. Valid calibration
survives AHRS, device-clock and reconnect resets. Batch and timed-sample flags
include `CONTROLLER_IMU_ACCELEROMETER_CALIBRATED` when applied.

## Verification

Build with `OVERRIDE_CONTROLLER_BUILD_TESTS=ON`. The calibration test checks
all six gravity directions, real acceleration, resets, physical device lookup
and malformed scale rejection. The Gear VR stream test checks corrected timed
forces and flags across a device-clock reset. Existing provider, gravity and
delivery tests remain applicable.

Validate the fitted values on separate stationary data, then repeat movement
with both controllers' acceleration integration enabled. Sensor calibration
reduces force bias; it does not measure constant Bluetooth/camera delay or
remove attitude, launch-velocity and sensor-to-grip offset errors. Keep the
normal prediction timeout, speed, path and distance limits.
