# Override Controller Configs

This directory intentionally contains no active default controller mapping.

Controller mappings are user/device-specific. Create them through the training flow and store them under:

```text
~/.config/xr_tracking/override_controller/
```

Runtime launch wrappers may point `CONFIG_PATH` to the selected mapping file.

## Per-device input timing

Config schema version 5 stores IMU routing, per-device orientation conversion/offset, and pulse/hold behavior inside each physical device:

```json
{
  "devices": [
    {
      "id": 1,
      "name": "VR-PARK",
      "imu_side": "none",
      "orientation_transform": {
        "enabled": false,
        "invert_x": false,
        "invert_y": false,
        "invert_z": false,
        "basis_rotation": {
          "rx_deg": 0.0,
          "ry_deg": 0.0,
          "rz_deg": 0.0
        }
      },
      "orientation_offset": {
        "enabled": false,
        "multiply_order": "post",
        "quaternion_xyzw": [0.0, 0.0, 0.0, 1.0]
      },
      "input": {
        "rel_axis_hold_ms": 0,
        "rel_button_hold_ms": 0,
        "button_hold_ms": 0,
        "button_release_grace_ms": 0,
        "pulse_mode": false,
        "dpad_pulse_gap_ms": 0,
        "dpad_release_ms": 0,
        "button_pulse_gap_ms": 0,
        "button_release_ms": 0,
        "button_pulse_startup_ms": 0,
        "button_pulse_startup_release_ms": 0,
        "button_pulse_startup_types": [],
        "hold_toggle_debounce_ms": 0
      }
    }
  ]
}
```

`imu_side` accepts `left`, `right`, or `none`. IMU routing is independent from
button bindings, so an IMU-only provider may be assigned to a controller side.
During Gear VR training it is filled automatically. Legacy Gear VR configs are
migrated from their unambiguous binding side and saved as schema version 5.

`orientation_transform` is applied to all selected device IMU vectors and to the
orientation basis before publishing. It is independent for each physical controller,
so left and right may use different axis inversions or basis rotations.

`orientation_offset` is applied afterwards to the published orientation quaternion only.
Use `post` (the default) for a fixed controller/grip presentation offset:
`q_output = q_transformed * q_offset`. `pre` is also accepted for a world-space correction.
The offset does not rotate angular velocity, acceleration, or magnetic-field vectors.
Missing fields keep the identity offset.

Gear VR touch coordinates and the physical pad click are separate inputs. The
provider publishes capacitive contact as `BTN_TOUCH`/`thumbstick_touch`; legacy
Gear VR configs receive that binding automatically. `absolute_stick` is the
default touchpad mode, so touching a position produces a stick value without
physically clicking the pad.

Every missing timing field defaults to `0`; `pulse_mode` defaults to `false`,
and `button_pulse_startup_types` defaults to an empty list. The old timing
fields under the top-level `input` object are not used by schema version 5.
