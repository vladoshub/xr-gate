# Override Controller

Input backend that maps physical controller inputs into the `controller_input` stream.

Supported input providers:

- Linux: evdev keyboard/gamepad/Bluetooth HID devices.
- Windows: Raw Input keyboard/mouse/HID devices, XInput gamepads/controllers, plus keyboard polling fallback.

`xr_runtime_adapter` can combine this physical input with hand tracking poses, synthetic fallback poses, or gesture-derived controls.

## Main stream

```text
/tmp/tracking_streams.json : controller_input
```

## Package output

```text
out/xreal_ultra/bin/override_controller/
```

## User configs

Controller mappings are user-specific and normally live under:

```text
~/.config/xr_tracking/override_controller/
```


## Windows XInput

On Windows the native provider scans a keyboard pseudo-device and connected XInput controllers (`xinput://0` .. `xinput://3`).

```powershell
.\out\xreal_ultra\bin\override_controller\override_controller.exe --list-devices
.\out\xreal_ultra\bin\override_controller\override_controller.exe --train --publish-transport tcp
```

The XInput provider emits:

```text
XINPUT_A/B/X/Y, shoulders, dpad, start/back, thumb clicks
XINPUT_LEFT_TRIGGER / XINPUT_RIGHT_TRIGGER
XINPUT_THUMB_LX/LY/RX/RY
```

For the current Windows pipeline use TCP publishing on `127.0.0.1:45672`; `xr_runtime_adapter` consumes it through `--controller-input tcp`.


## Windows Raw Input

On Windows the native provider registers a hidden Raw Input window and scans per-device Raw Input handles. This is preferred for Bluetooth buttons, keyboard-like remotes, mice, and HID consumer-control devices because mappings can be tied to the actual source device instead of the global keyboard state.

Device backends shown by `--list-devices` may include:

```text
rawinput_keyboard
rawinput_mouse
rawinput_hid
keyboard              # polling fallback
xinput
```

Raw mouse movement is emitted as relative axes. Raw HID reports are exposed as bit-level key events (`RAW_HID_BIT_N`) using report-byte diffs; this is intentionally generic and should be replaced later with HID usage parsing if a specific controller needs semantic names.

## Alternative layout

During `--train`, the first optional prompt can capture one physical button as a global layout switch. If configured, the normal layout is trained first, and the trainer then offers an alternative layout. The alternative layout has the same structure as the default layout: normal bindings plus optional long-press toggle bindings.

At runtime, a normal press on the layout switch toggles between the default and alternative layouts for both controller sides at once. The switch button is reserved during training and is not accepted as a controller action binding.

## Config device registry and reconnect

New configs store device fingerprints once in a top-level `devices` list. Bindings reference the device by `device_id` instead of duplicating the full device fingerprint in every action.

Existing configs with inline `device` blocks are still readable. When they are saved again, they are normalized to the `devices` / `device_id` format.

To update device fingerprints after Bluetooth reconnects, event path changes, or when a default config only has device names:

```bash
./override_controller --config ~/.config/xr_tracking/override_controller/default.json --connect-devices
```

The command lists currently readable input devices, shows the devices used by the config with their `left` / `right` usage, and asks you to press any button on each configured physical device. It then writes the refreshed device fingerprints back to the config.
