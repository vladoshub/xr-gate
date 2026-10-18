# XR Gate in Docker: TCP runtime

[Русский](README.ru.md) · English · **[Offline guide](START-HERE.html)**

This is the repository-root **docker/** directory. Run the commands below from
this directory. The image downloads pinned **XR Gate v2.3.0 Ubuntu 24.04 x64**
runtime and separate Mercury models from GitHub, checks SHA-256 and uses the
release's dependency installer. It does not compile XR Gate. The release needs
an **AVX2 / x86-64-v3** CPU.

Windows setup: **[Docker Engine inside Ubuntu WSL](docs/WINDOWS.en.md)**.
Controller setup: **[Gear VR / VR-PARK](docs/CONTROLLERS.en.md)**.

## Architecture and localhost ports

The container runs the normal `xr_client` console, capture, Basalt, Mercury and
runtime adapter. Internal streams retain their normal SHM transport. Application
TCP is enabled at port **45810**, protocol **xr_gate_app_v1**, JSON Lines.

| Connection | Address |
| --- | --- |
| Windows/Linux host application → runtime | 127.0.0.1:45810 |
| Container listener | 0.0.0.0:45810 inside its private network namespace |
| Optional camera-video client | 127.0.0.1:45700 when explicitly enabled |

Compose publishes on host **127.0.0.1** only. Use regular local **Docker Engine
28+** in the same Linux/WSL system that owns the USB nodes. Docker Desktop,
remote/rootless engines are not supported by this USB workflow. The scripts do
not open LAN ports or use host networking.

## First run on Linux

Install Docker Engine and its Compose plugin if needed:

```bash
sudo bash scripts/install-engine-ubuntu.sh
```

From `docker/`:

```bash
cp --no-clobber .env.example .env
sudo ./xr-gate build
sudo ./xr-gate check-image
sudo ./xr-gate doctor
sudo ./xr-gate run
```

Connect XREAL before the hardware check/run. Keep the resulting **xr_client
console open**, complete its startup gate and display-mode prompts. On WSL, use
the current display mode if SBS is already configured in Windows/on the glasses.
In another terminal:

```bash
./xr-gate check-tcp
```

For a background container, use `sudo ./xr-gate up`, then
`sudo ./xr-gate console`. Stop with Ctrl+C in the running console or
`sudo ./xr-gate stop`. Use `sudo ./xr-gate logs` or `sudo ./xr-gate shell` for
logs/an interactive shell. `console` uses Docker's attach mechanism; normal
Docker detach keys are Ctrl+P then Ctrl+Q.

## Release defaults and host configuration

Without overrides, the container uses the release's `xreal_ultra` profile.
The entire **config/** directory is mounted read-only. Optional files:

| Host path | Effect |
| --- | --- |
| config/xr_client.json | Replaces the selected client profile |
| config/override_controller.json | Copied to the override service's CONFIG_PATH inside the container |
| .env | Docker ports, USB IDs, paths, profile and optional features |

For Gear VR, `CONFIG_PATH` is normally `~/.config/xr_tracking/override_controller/gearvr.json`;
ordinary evdev/VR-PARK uses `default.json`. A differing old destination is backed
up. Controller state is persisted in a named volume. An active host override is
copied again on every startup: export registration/training results back to it
before restarting, as described in the controller guide.

Export release templates without overwriting an existing export directory:

```bash
sudo ./xr-gate export-configs
cp --no-clobber exported-configs/xr_client/xreal_ultra.json config/xr_client.json
```

Edit only the needed fields in that copied profile. A controller override alone
does not select providers or enable the service. Restart the container after
configuration changes. Set **XR_CONFIG_DIR** in `.env` for another host config
directory. Use literal `KEY=value` entries, quote paths containing spaces, and
avoid shell expressions.

## XREAL USB

The default selector is **3318:0426**. The helper passes only that device's
video/HID/raw-USB nodes, not all of `/dev` or every USB device. Its first UVC
capture node maps to **/dev/video0 inside the container**, preserving the release
profile even if the host uses `/dev/video8`. Set **XR_CAMERA_DEVICE** to select a
specific capture node of the same USB identity.

USB selectors come from `.env.example`, overridden by `.env` and the shell.
There is no vendor identity embedded in the orchestration code. For another
device, set `XR_USB_IDS` to its comma-separated VID:PID pairs and select its
runtime profile/config. Selected USB devices also expose their `ttyACM*` and
`ttyUSB*` serial nodes. `XR_SERIAL_DEVICES=/dev/serial/by-id/<id>` (or explicit
`/dev/ttyACM0,/dev/ttyUSB0`) adds only the named serial devices, including an IMU
with a USB identity different from the camera.

`XR_REQUIRE_CAMERA=1` checks for a local camera. HID is optional by default;
set `XR_REQUIRE_HIDRAW=1` for profiles that need it. For a remote/TCP-only
profile, use `XR_USB_IDS=`, `XR_REQUIRE_CAMERA=0`, and leave other local device
selectors empty. An explicit `XR_CAMERA_DEVICE=/dev/videoN` can select a camera
without `XR_USB_IDS`; this mode does not discover other cameras/HID/USB devices.
Listed nodes must exist and be character devices; the whole `/dev` is never
mounted. Match capture/IMU paths in the runtime config to the container mapping.

After reconnecting USB, Windows sleep, or `usbipd attach`, **stop and recreate
the container** with this helper. `docker restart` does not refresh device
mappings. The kernel/drivers belong to the Linux/WSL host; missing UVC/HID nodes
must be fixed there first.

## Bluetooth

BlueZ runs on the Linux host. On Windows, forward a dedicated USB Bluetooth
dongle into Ubuntu WSL. Pair/trust controllers through the host's `bluetoothctl`,
then set **XR_BLUETOOTH=1** in `.env`. The container accesses BlueZ through the
mounted system D-Bus socket. It does not run another `bluetoothd`.

For ordinary evdev devices, explicitly list only the required event nodes in
**XR_INPUT_DEVICES**, for example `/dev/input/event12,/dev/input/event13`.
Recreate the container if node names change. Windows XInput/DirectInput access
alone does not create Linux event nodes.

## Optional stereo camera video

By default only application TCP is published. For a separate binary video
client, set **XR_VIDEO_TCP=1** in `.env`, then recreate the container. This
publishes **127.0.0.1:45700** and selects `VIDEO_OUTPUT=both`. The video backend
still follows the release profile's console control, normally command **6**.
Use XR Gate's binary stereo-video client; this is not RTSP/H.264 and does not
carry the SteamVR game's rendered display.

## Windows SteamVR driver

The patched CI separately builds artifact **xr-gate-steamvr-windows-x64**.
Extract its **60Hz-direct** ZIP, close SteamVR and run **Install-Driver.bat** in
the inner `xr_tracking` folder. With this container running, run **Test-TCP.bat**,
then start SteamVR. See the [complete Windows/SBS/Direct Mode guide](docs/STEAMVR.en.md).
Actual Direct Mode support depends on the XREAL display/GPU/video connection.

Do not start direct WSL XR Gate and this container on the same host TCP port at
the same time. The Windows driver defaults to `127.0.0.1:45810`.

## Diagnostics and validation

```bash
sudo ./xr-gate doctor
sudo ./xr-gate config
sudo ./xr-gate logs
lsusb
ls -l /dev/video* /dev/hidraw*
bluetoothctl list
rfkill list
sudo modprobe uvcvideo
sudo modprobe hid_generic
sudo modprobe btusb
```

The helper checks the local Engine, USB nodes and, when enabled, BlueZ/D-Bus.
Tracking issues after the startup gate require checking light, calibration and
release logs. Configurations are from the pinned release, not a new calibration
for every physical device. Tests/limits are listed in **docs/VALIDATION.md**;
the actual Docker daemon, Windows/WSL, USB/Bluetooth and GPU path require hardware
validation. No remote CI run or publication is performed by this patch delivery.
