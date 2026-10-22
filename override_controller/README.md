# Override Controller

Input backend that maps physical controller inputs into the `controller_input` stream.

Supported input providers:

- Linux: evdev keyboard/gamepad/Bluetooth HID devices.
- Linux: XIAO nRF54L15 Sense serial controllers using `xr_controller_v1`.
- Linux: Samsung Gear VR Controller through native BlueZ BLE.
- Windows: Raw Input keyboard/mouse/HID devices, XInput gamepads/controllers, plus keyboard polling fallback.

`xr_runtime_adapter` can combine this physical input with hand tracking poses, synthetic fallback poses, or gesture-derived controls.

Gear VR sensor timestamps, buffered IMU delivery and collection settings are
described in [Gear VR IMU timing](docs/gearvr_imu_timing.md).

Linux Gear VR startup waits for the VR-mode `08 00` notification before sending
SENSOR, with a three-second fallback for firmware that does not expose the ack.
GATT write completion alone does not signal mode application. A sensor packet
refreshes a two-second liveness deadline throughout the connection: a stalled
stream gets one SENSOR retry, followed by notification-path recovery if it stays
silent. Short control replies do not refresh IMU liveness or enter pose decoding.
The protocol/timing and native BlueZ lifecycle tests run in **Controller input tests**;
that workflow also publishes an Ubuntu 24.04 WSL hotfix with backup/rollback support.

## Main stream

```text
/tmp/tracking_streams.json : controller_input
```

## Package output

```text
out/xr-gate/bin/override_controller/
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

The command lists currently readable input devices, shows the devices used by the config with their `left` / `right` usage, and asks you to press any button on each configured physical button device. Pure IMU-only entries are not sent through the button-capture prompt.

At the end of both `--train` and `--connect-devices`, the tool checks for detected IMU-capable devices that are not assigned to either side. The optional IMU assignment step is shown only when at least one such device exists and at least one controller side has no IMU. For each missing side, choose a listed IMU device or press Enter/type `skip`; skipping leaves that side unchanged and does not create a config device. The resulting fingerprint is stored in `devices[]` with `imu_side: left` or `imu_side: right`, so the IMU does not need to expose any buttons.

The command then writes the refreshed device fingerprints and any accepted IMU assignments back to the config.

## Per-device pulse and hold settings

Pulse filtering and synthetic hold timings are configured independently in
`devices[].input`. This allows two identical or mixed controller models to use
different event timing. Missing settings use zero/disabled defaults; there is no
global timing fallback from the launcher script.

## XIAO nRF54L15 serial provider

The native `xiao_nrf54l15` provider reads unchanged 64-byte
`xr_controller_v1` (`XCTL`) IMU samples and periodic 32-byte `XCID` identity
frames directly from the board's SAMD11 USB CDC serial port. It validates version, embedded packet size, IEEE CRC32 and finite
IMU values, performs stream resynchronization after corruption, maps the device
microsecond timestamp into the host monotonic clock, and feeds the shared
`ControllerImuProcessor`/Madgwick pipeline.

Hardware timestamp fields, buffered delivery and serial clock behaviour are
documented in [docs/xr_controller_imu_timing.md](docs/xr_controller_imu_timing.md).

List detected boards:

```bash
PROVIDERS=xiao_nrf54l15 \
  override_controller/scripts/linux/start_override_controller.sh \
  --list-devices
```

Use it together with normal evdev controllers:

```bash
PROVIDERS=evdev,xiao_nrf54l15 \
  override_controller/scripts/linux/start_override_controller.sh
```

Automatic discovery checks `/dev/serial/by-id/*` first and then `/dev/ttyACM*`.
For deterministic selection, especially on a development machine with other
CDC devices, pass one or more comma-separated ports:

```bash
PROVIDERS=evdev,xiao_nrf54l15 \
PROVIDER_OPTIONS='xiao_nrf54l15.ports=/dev/serial/by-id/usb-left,/dev/serial/by-id/usb-right' \
  override_controller/scripts/linux/start_override_controller.sh
```

Provider options:

| Option | Default | Meaning |
| --- | ---: | --- |
| `ports` | automatic | Comma-separated serial paths |
| `baud_rate` | `230400` | UART line speed |
| `initial_scan_ms` | `1200` | Initial protocol-detection window |
| `reconnect_ms` | `1000` | Serial rescan/reopen interval |
| `stale_ms` | `250` | IMU stale timeout |
| `axis_flat` | `1024` | Dead zone reported for analog bindings |
| `madgwick_beta` | `0.04` | Shared 6DoF AHRS gain |

The provider exposes a stable fingerprint with
`backend=xiao_nrf54l15`. After receiving `XCID`, `uniq` is
`xiao_nrf54l15:uid:<hardware-uid>` and remains stable across USB ports and
`ttyACM` numbering. Older firmware without `XCID` keeps the previous fallback:
USB serial number, `/dev/serial/by-id`, `/dev/serial/by-path`, then the explicit
port.

Current firmware publishes IMU data with the controls-valid flag clear. Such a
board appears in `--list-devices` and publishes IMU, but cannot generate a
button-training event until GPIO controls are added. The final optional step of
`--train` and `--connect-devices` lists these unassigned IMU-only devices and can
assign one to `left` or `right` without requiring a button press. The stored
config entry has the following form:

```json
{
  "id": 3,
  "platform": "linux",
  "backend": "xiao_nrf54l15",
  "uniq": "xiao_nrf54l15:uid:0123456789abcdef",
  "imu_side": "left",
  "orientation_transform": {
    "enabled": true,
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
  }
}
```

When future firmware sets `controls valid`, the already-reserved fields are
published as normal training inputs:

| `xr_controller_v1` control | Provider event |
| --- | --- |
| A / B / C | `EV_KEY / XCTL_BUTTON_A/B/C` |
| Trigger / grip / menu | `EV_KEY / XCTL_TRIGGER/GRIP/MENU` |
| Stick click | `EV_KEY / XCTL_STICK_CLICK` |
| D-pad | `EV_KEY / XCTL_DPAD_*` |
| Thumbstick X/Y | `EV_ABS / XCTL_THUMBSTICK_X/Y` |
| Analog trigger/grip | `EV_ABS / XCTL_TRIGGER_AXIS/GRIP_AXIS` |

One serial stream must have one reader. Do not point `capture_service_cpp` and
`override_controller` at the same physical `/dev/ttyACM*` device at the same
time; select which process owns that controller's serial stream.


### Per-controller IMU orientation offset

Each `devices[]` entry may apply a fixed presentation offset after its
`orientation_transform`:

```json
"orientation_offset": {
  "enabled": true,
  "multiply_order": "post",
  "quaternion_xyzw": [0.0, 0.0, 0.0, 1.0]
}
```

`post` is intended for the fixed IMU-to-controller/grip orientation. With the
native optical-grip vector fix, gyro, force and magnetic vectors are rotated
by the inverse local offset so they remain consistent with the new orientation.
Calibrate a neutral pose on each side with
`debug/calibrate_controller_orientation_offset.py` while the existing offset is
disabled, or pass the matching config with `--replace-existing-offset` so the
tool can remove the currently configured offset from the observed stream.

Standalone calibration, ready-to-copy JSON only:

```bash
python3 debug/calibrate_controller_orientation_offset.py \
  --side left \
  --registry /tmp/tracking_streams.json \
  --stream controller_input
```

Calibrate and write the unique `devices[]` entry assigned to the side:

```bash
python3 debug/calibrate_controller_orientation_offset.py \
  --side left \
  --registry /tmp/tracking_streams.json \
  --stream controller_input \
  --config ~/.config/xr_tracking/override_controller/default.json \
  --replace-existing-offset \
  --write
```

Restart `override_controller` after writing the config.

For guided calibration against Mercury optical wrist orientation, use
`debug/calibrate_grip.sh`. It records both sides independently, compares repeated
multi-axis movement rounds and removes any active provider offset before fitting
an absolute replacement. See [Optical grip calibration](docs/imu_optical_grip_calibration.md)
for dependencies, runtime prerequisites, capture, validation and application.
The tools are also installed in `bin/override_controller/tools`.

## Samsung Gear VR Controller BLE provider

On Linux, stopping the provider sends the controller's OFF command, stops
notifications and calls BlueZ `Device1.Disconnect` for each Gear VR connection
it adopted or attempted. Ctrl+C, SIGTERM, SIGHUP and normal exception unwinding use this
cleanup. Pending BlueZ Connect requests are canceled too. Shutdown D-Bus calls
use short timeouts so an unresponsive controller cannot block the other side.

When launched by xr-client, each process receives its own temporary cleanup
file. The transport records the exact BlueZ device paths before connecting and
acknowledges successful disconnects. After process exit, xr-client disconnects
any remaining paths using `busctl`, including after a crash, SIGKILL or a forced
manual stop. This happens before an automatic restart; the fallback has a
three-second overall budget. No Bluetooth device enumeration is used during
cleanup. The installed binary and xr-client Python files must both be updated.

Crash recovery requires xr-client to remain alive and `busctl` to be available
with permission to call BlueZ. xr-client also cleans up on SIGTERM and SIGHUP
(terminal close). A standalone provider killed without unwinding, or SIGKILL
of xr-client along with the provider, cannot run this fallback. Pairing
and trust settings are retained. The next provider start connects normally.

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

With multiple Bluetooth adapters, the Linux provider ignores unpowered radios
and selects one adapter-specific BlueZ record per controller. It initially
prefers an already connected record and pins that route until the provider
restarts. Old pairings on another adapter cannot reset the working sensor
stream or redirect reconnect attempts. The chosen adapter name, address and
BlueZ path are printed in `override_controller.log`.

To select an adapter explicitly, set `XR_BLUETOOTH_ADAPTER` to its Bluetooth
address, `hciN` name, or `/org/bluez/hciN` path before starting XR Gate. Prefer
the address because `hciN` numbering can change after reboot. The selector
restricts discovery, reconnects and exit cleanup to that adapter; it does not
change adapter power or transfer pairings. Pair the controllers on that adapter
first. After changing adapters, restart `override_controller`; an unavailable
pinned adapter does not trigger automatic takeover through another radio.

```bash
bluetoothctl list
export XR_BLUETOOTH_ADAPTER='AA:BB:CC:DD:EE:FF'  # Adapter address, not controller address.
./run_xr_client.sh --config xreal_ultra
```

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
and both axes return to zero when the finger leaves the pad. Provider-specific
settings use the common `provider.key=value` interface:

```bash
PROVIDER_OPTIONS='gearvr_ble.touchpad.mode=absolute_stick;gearvr_ble.touchpad.deadzone=0.12;gearvr_ble.touchpad.radius=90;gearvr_ble.touchpad.invert_x=0;gearvr_ble.touchpad.invert_y=1;gearvr_ble.madgwick_beta=0.04;gearvr_ble.reconnect_ms=1000' \
PROVIDERS=evdev,gearvr_ble \
  override_controller/scripts/linux/start_override_controller.sh
```

`touchpad.mode` accepts `absolute_stick`, `relative_stick`, `dpad`, or `raw`.
The legacy `GEARVR_*` and `OVERRIDE_CONTROLLER_GEARVR_*` environment variables
remain supported, but they are parsed inside `GearVrInputProvider`; the common
launcher and argument parser do not contain Gear VR-specific option handling.

### IMU output

The native provider decodes gyroscope, accelerometer, and magnetometer values,
performs a confirmed stationary gyroscope-bias estimate, and computes a 6DoF
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
bindings. An IMU on a controller used during normal binding training is assigned
automatically. Unused/buttonless IMU devices are offered in the final optional
IMU assignment step for `--train` and `--connect-devices`; Enter/`skip` preserves
the current side configuration. This allows serial/BLE IMU-only providers to
publish controller orientation without exposing any buttons.

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

## Backend gravity control

`override_controller` reads the same backend control JSON as `xr_runtime_adapter`:

```json
{
  "gravity_magnitude": 9.80665,
  "reset_counter": 0
}
```

The Linux launcher uses:

```bash
XR_BACKEND_CONTROL_FILE=/tmp/xr_backend_control.json
OVERRIDE_CONTROLLER_BACKEND_CONTROL_POLL_MS=500
```

`gravity_magnitude` is used both when Gear VR raw accelerometer counts are converted
from `LSB/g` to `m/s²` and by the shared IMU processor's Madgwick/stationary gates.
Changing gravity is applied without restarting the service. `reset_counter` is tracked
for diagnostics; runtime prediction/yaw reset handling remains in `xr_runtime_adapter`.


## Startup gyroscope zeroing

Start the controller service with each controller resting on a stable surface.
Wait for `Both controller gyroscopes zeroed` in the XR Gate console before taking
them in your hands and starting the separate optical motion calibration. For a
single controller, wait for its individual `gyroscope zeroed` message.

The common IMU processor accepts one continuous stationary window, commits its
bias atomically, and freezes that bias until a device/AHRS reset. Motion, invalid
samples, sample gaps, changing gravity and excessive mean rotation or noise
restart the candidate. Pending samples retain their timestamps, buttons, gyro
and force, but orientation is not advertised as runtime-ready before zeroing.
There is no timeout that silently accepts a moving controller. The provider log
contains throttled `[override_controller][gyro-zero]` progress, accepted bias in
native sensor axes, and the device epoch. A disconnect/reconnect or accelerometer
calibration change requires a fresh window.

Both `gearvr_ble` and `xiao_nrf54l15` accept these provider options. All angles in
this table use radians, and all force values use SI units; no USB IDs, models or
mounting angles enter the estimator.

| Option after the provider prefix | Default | Meaning |
| --- | ---: | --- |
| `gyro_bias.stable_ms` | `1500` | Continuous stationary sample time |
| `gyro_bias.min_samples` | `100` | Minimum distinct samples in that window |
| `gyro_bias.max_gap_ms` | `50` | Maximum device-sample interval during acquisition |
| `gyro_bias.max_sample_speed_rad_s` | `0.20` | Reject individual large rotations |
| `gyro_bias.max_bias_rad_s` | `0.01745329252` | Maximum accepted mean bias, 1 degree/s |
| `gyro_bias.max_gyro_stddev_rad_s` | `0.05` | Maximum combined gyro standard deviation |
| `gyro_bias.max_force_error_m_s2` | `1.0` | Maximum difference from gravity magnitude |
| `gyro_bias.max_force_range_m_s2` | `0.35` | Maximum vector range of force over the window |

For example, a device profile can specify:

```bash
export PROVIDER_OPTIONS='gearvr_ble.gyro_bias.stable_ms=2000;gearvr_ble.gyro_bias.max_bias_rad_s=0.01745329252'
```

Keep any other existing provider options in that value. The same settings can be
passed as repeated `--provider-option` arguments. Invalid, nonfinite or
inconsistent limits fail at provider creation, before connecting to hardware.

A 6DoF IMU cannot distinguish arbitrarily slow constant yaw from sensor bias
without an external reference. A stable window and bounded accepted mean reject
common startup movements, but the controller must still actually rest during
zeroing. Sensors with a larger measured stationary bias or noise need explicit
profile limits. Optical yaw confirmation, optical drift correction and saved
accelerometer/mounting calibration are separate from this startup estimate.
