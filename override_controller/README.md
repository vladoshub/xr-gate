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

## Per-device pulse and hold settings

Pulse filtering and synthetic hold timings are configured independently in
`devices[].input`. This allows two identical or mixed controller models to use
different event timing. Missing settings use zero/disabled defaults; there is no
global timing fallback from the launcher script.

## Samsung Gear VR Controller BLE provider

`override_controller` can aggregate the normal platform input provider and the
Gear VR BLE provider in the same process. Gear VR support is opt-in. The provider
is fully native C++; Python and Bleak are not used. Common packet decoding,
touchpad handling, IMU/AHRS, training, and binding logic are OS-independent.
Linux currently supplies the working BlueZ/system-D-Bus transport:

```bash
PROVIDERS=evdev,gearvr_ble \
  override_controller/scripts/linux/start_override_controller.sh
```

The controller must be paired and trusted once in BlueZ. The provider then
finds paired devices whose name matches `Gear VR Controller(...)`, connects to
them automatically, verifies the Gear VR GATT service/characteristics, enables
VR sensor mode, and keeps reconnecting after sleep or signal loss. No MAC
address is entered in the launcher or JSON by hand.

```bash
bluetoothctl
scan on
pair AA:BB:CC:DD:EE:FF
trust AA:BB:CC:DD:EE:FF
quit
```

Wake the controller with a button press, then run normal training. The existing
trainer receives provider events in Linux input-code form, so Gear VR bindings,
alternative layouts, hold-toggle mappings, and layout switching use the same
configuration format as evdev:

```bash
PROVIDERS=evdev,gearvr_ble TRAIN=1 CONFIG_NAME=gearvr \
  override_controller/scripts/linux/start_override_controller.sh
```

A trained fingerprint uses `backend=gearvr_ble` and
`uniq=gearvr_ble:<Bluetooth address>`. This stable identity restores the same
physical controller and controller side even when two identical controllers
connect in a different order.


### Provider/transport split

```text
GearVrInputProvider (common C++)
├── packet decoder and Gear VR command sequence
├── button/touchpad conversion
├── ControllerImuProcessor / Madgwick
├── ControllerInputV3 and training/bindings
└── BleTransport
    ├── Linux: BlueZ + system D-Bus
    └── Windows: C++/WinRT backend placeholder
```

The transport contract exposes only paired-device snapshots and raw BLE
notification packets. Adding Windows support does not require changes to the
Gear VR protocol, touchpad modes, AHRS, configuration, or trained bindings.

### Exposed input codes

| Gear VR control | Provider event |
| --- | --- |
| Trigger | `EV_KEY / BTN_TRIGGER` |
| Touchpad physical click | `EV_KEY / BTN_LEFT` |
| Touchpad capacitive contact | `EV_KEY / BTN_TOUCH` |
| Back | `EV_KEY / KEY_BACK` |
| Home | `EV_KEY / KEY_HOMEPAGE` |
| Volume up/down | `EV_KEY / KEY_VOLUMEUP`, `KEY_VOLUMEDOWN` |
| Touchpad movement | `EV_ABS / ABS_X`, `ABS_Y` |

The touch surface defaults to `absolute_stick`: the physical pad center is the
virtual stick center, capacitive contact is independent from the physical click,
and both axes return to zero when the finger leaves the pad. Other modes:

```bash
GEARVR_TOUCHPAD_MODE=absolute_stick  # default; capacitive position, no physical click required
GEARVR_TOUCHPAD_MODE=relative_stick  # first touch point becomes the temporary center
GEARVR_TOUCHPAD_MODE=dpad            # KEY_UP/DOWN/LEFT/RIGHT
GEARVR_TOUCHPAD_MODE=raw             # normalized absolute touch position
```

Relevant tuning variables:

```bash
GEARVR_TOUCHPAD_DEADZONE=0.12
GEARVR_TOUCHPAD_RADIUS=90
GEARVR_TOUCHPAD_INVERT_X=0
GEARVR_TOUCHPAD_INVERT_Y=1
GEARVR_MADGWICK_BETA=0.04
GEARVR_RECONNECT_MS=1000
```

### IMU output

The native provider decodes gyroscope, accelerometer, and magnetometer values,
performs a short stationary gyroscope-bias estimate, and computes a 6DoF
orientation with the shared C++ `ControllerImuProcessor`. Raw magnetic field
is published, but is intentionally not used by the AHRS until a proper
magnetometer-calibration flow is added. IMU routing is configured explicitly in
the top-level device entry:

```json
{
  "id": 1,
  "backend": "gearvr_ble",
  "imu_side": "left"
}
```

`imu_side` accepts `left`, `right`, or `none` and is independent from button
bindings. Gear VR training assigns it automatically. This also allows a future
MPU-6050 provider to publish IMU-only data without exposing any buttons.

The runtime adapter still requires the matching side to use:

```bash
RUNTIME_CONTROLLER_LEFT_ORIENTATION_SOURCE=IMU_OVERRIDE_CONTROLLER_RUNTIME
RUNTIME_CONTROLLER_RIGHT_ORIENTATION_SOURCE=IMU_OVERRIDE_CONTROLLER_RUNTIME
```

and it dynamically falls back to hand tracking whenever the provider does not
publish current valid IMU orientation.

The provider boundary and shared C++ IMU processor are intentionally transport
independent. A future MPU-6050 provider only needs to discover/open its own
serial/BLE device, fill `imu::RawControllerImuSample` in SI units, and publish
the same normal input/IMU provider state.
