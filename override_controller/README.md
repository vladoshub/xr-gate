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
