# Override Controller Configs

This directory intentionally contains no active default controller mapping.

Controller mappings are user/device-specific. Create them through the training flow and store them under:

```text
~/.config/xr_tracking/override_controller/
```

Runtime launch wrappers may point `CONFIG_PATH` to the selected mapping file.

## Per-device input timing

Config schema version 2 stores pulse/hold behavior inside each physical device:

```json
{
  "devices": [
    {
      "id": 1,
      "name": "VR-PARK",
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

Every missing field defaults to `0`; `pulse_mode` defaults to `false`, and
`button_pulse_startup_types` defaults to an empty list. The old timing fields
under the top-level `input` object are not used by schema version 2.
