# XR Gate on Windows (installation via WSL2)

[English](README.md) · [Русский](README.ru.md)

## Desktop launcher

Extract the complete **xr-gate-windows-wsl-usbip.zip** and open **XR-Gate.exe**. The launcher is a Windows x64 WPF application with .NET 10 included; users do not need to install .NET or its SDK. Keep the executable with the supplied scripts and subfolders.

1. Open **Settings**. Select the hardware preset and enable Bluetooth if using BLE controllers. The supplied preset selects ASUS USB-BT500 by VID:PID when Bluetooth BUSID is empty; use **List USB devices** for manual selection or another adapter. The list appears directly below the button in Settings; select and copy the required BUSID. If usbipd is missing, run **Install / continue** first. Device identifiers, Linux modules, geometry, optics, controller templates and display matching remain in `hardware/*.env`, with overrides in `.env`.
2. Click **Install / continue**. Follow the displayed Windows restart, first Ubuntu login or Ubuntu restart instructions, then continue installation. Administrator rights are requested only for prerequisite installation and initial USB binding.
3. For desktop SBS output, enable SBS on the glasses and **Extend these displays** in Windows. Close SteamVR and use **Select glasses display** if the glasses have not been identified automatically.
4. Click **Start VR**. Complete startup/calibration prompts in the tracking console; SteamVR opens after fresh valid head poses arrive. The launcher keeps the selected WSL distribution running.
5. Use **Stop tracking & return USB** to cleanly stop the managed runtime and return its devices to Windows. Closing the launcher also requests a clean stop; it does not terminate the entire WSL distribution or unrelated applications.

**Automatic driver frequency** is enabled by default (`XR_AUTO_DRIVER_FREQUENCY=1`). Installation and Start VR select/register the driver matching the glasses' current Windows refresh rate. If there is no matching variant, the launcher selects **60 Hz** and shows a warning. When the glasses output is unambiguous, it also tests and applies 60 Hz to that output without changing resolution. If Windows cannot apply it, startup stops with instructions. An unknown/ambiguous output uses the 60 Hz driver with a warning and requires explicit display selection before desktop VR; the main monitor is never guessed. Display identification uses an explicit output name in `.env`, the saved display identity, or a unique `XR_DISPLAY_MATCH` from the hardware preset. Resolution alone is never used to identify the glasses. Direct Mode may hide the output, in which case 60 Hz is the fallback. Disable automatic selection to force a manual frequency.

**Diagnostics** and **Refresh status** can be cancelled and have a 60-second time limit. Individual Linux diagnostic commands have a 10-second limit; a stalled probe is reported and the remaining checks continue. Status reports the configured driver frequency separately from the current Windows mode of the identified glasses output. If no output is identified, its current refresh rate remains unknown.

**Run** contains USB attachment, Bluetooth discovery and left/right controller selection. **tools** contains Diagnostics, controller/driver registration and head-tracking checks. Display selection and controller training open interactive consoles. **Diagnostics** and **Save log** provide troubleshooting output. Advanced `.env` settings and comments are preserved when saving the GUI settings; reopen the launcher after editing `.env` externally.

**Attach USB devices** has a **Cancel USB operation** button. Closing the window also cancels the current USB operation and its transient child processes. Each `usbipd` command has a 30-second limit (`XR_USB_COMMAND_TIMEOUT`, 5–120 seconds, in `.env`); the desktop USB action has a three-minute overall limit. Cancellation or a timeout may leave devices attached by earlier steps: check **List USB devices**, reconnect the selected adapter if necessary, and retry. BUSIDs are refreshed from the selected Windows instance before attach. With automatic Bluetooth selection, leave its BUSID empty so a reboot or port change can be resolved through VID:PID.

Use **Browse…** to change the hardware preset. Inherited fields and the controller label update immediately; deliberate overrides are retained. The launcher saves changed fields instead of pinning every inherited default in `.env`. **List USB devices** remains available while tracking is active. **Check live head tracking** and USB listing have a Cancel button; partial TCP messages cannot extend the configured startup timeout.

If settings cannot load, the launcher opens **configuration recovery** with the problem file and repair/reload controls. **Back up & reset this file** preserves the original `.env` or saved-state file under a unique backup name. Hardware preset files are repaired or reselected. Recovery does not stop an existing runtime or return USB; stop its console with Ctrl+C before reinstalling after a state reset.

Bluetooth preparation now requires a real HCI controller when Bluetooth is enabled. `XR_BLUETOOTH_READY_TIMEOUT` controls the readiness wait (1–120 seconds); `XR_CONTROLLER_INITIAL_SCAN_MS` controls the initial controller scan (1–600000 ms). Both can be overridden in the hardware env or `.env`.

When updating the launcher, close it and replace the **whole archive**, including scripts and `windows/DeadlineLineReader.cs`. Keep your `.env`; run Install / continue when changing the release or driver variant. See [CHANGELOG.md](CHANGELOG.md) for launcher changes.

If installation reports an unavailable drive while locating SteamVR, check **Settings > SteamVR folder** (`XR_STEAMVR_PATH`). Clear it for automatic detection or enter the installed SteamVR folder. Old OpenVR/library paths on missing drives are skipped. The Linux installation can finish with a warning if registration must be deferred; afterward use **tools > Register SteamVR driver**.

### CI builds

- **Launcher configuration tests** runs the C# tests, a Windows PowerShell bridge test, publishes the self-contained executable and starts the actual WPF window in smoke mode. Its **xr-gate-windows-wsl-usbip-launcher** artifact contains the executable and installation scripts; Install downloads drivers/runtime from the selected release. Run this workflow manually to build the launcher without a full Linux rebuild.
- **XR Gate split build**, with **Build windows release (includes SteamVR driver)**, adds the same executable to **xr-gate-windows-wsl-usbip.zip** alongside all selected Windows drivers. A 60 Hz fallback is always included, even when only another frequency was requested.
- To build locally, install the .NET 10 SDK on Windows and run `powershell -NoProfile -ExecutionPolicy Bypass -File win/launcher/build.ps1` from the repository. Output: `out/windows-launcher/XR-Gate.exe`.

The command-line instructions below remain available.

## 1. Download and prepare

1. Use Windows 11 x64 and an AVX2-capable CPU.
2. Install Steam and SteamVR. Open SteamVR once, then close it.
3. Download **xr-gate-windows-wsl-usbip.zip** from [Releases](https://github.com/vladoshub/xr-gate/releases), or the **xr-gate-windows-wsl-usbip** artifact from your CI run.
4. Extract the ZIP to a permanent folder. Open its `xr-gate-windows-wsl-usbip` folder. Keep the `drivers`, `hardware`, `linux` and `windows` subfolders together.
5. Open a regular Windows PowerShell in this folder. Run all commands below there.

### Bluetooth adapter for WSL2

**Tested adapter: ASUS USB-BT500** (`0b05:190e`), with Ubuntu 24.04 in WSL2 and kernel `6.18.54.1-microsoft-standard-WSL2`.

**Use a dedicated USB Bluetooth adapter for XR Gate and leave the system adapter with Windows.** USB forwarding transfers the whole adapter to WSL; while attached, Windows cannot use it for Bluetooth keyboards, mice, headphones or other devices. Returning the adapter to Windows makes it available there again. See the [usbipd WSL documentation](https://github.com/dorssel/usbipd-win/wiki/WSL-support).

In the desktop launcher, enable **Enable Bluetooth**. For ASUS USB-BT500, clear **Bluetooth USB BUSID** and keep **Bluetooth VID:PID** at `0b05:190e`, as supplied by the XREAL preset. Save settings, then use **Attach USB devices**. The adapter's current BUSID is resolved automatically, including after changing USB ports.

For the scripts, set `XR_BLUETOOTH=1` and `XR_BLUETOOTH_USB_ID=0b05:190e` in `.env`, with `XR_BLUETOOTH_BUS_ID` empty. You can also pass `-BluetoothUsbId 0b05:190e` explicitly. The generic hardware template leaves VID:PID empty so another adapter can be configured.

**Selection priority:** explicit BUSID, explicit `XR_BLUETOOTH_INSTANCE_ID`, configured VID:PID, then the saved adapter when both selectors are empty. An explicit BUSID must exist; it never silently falls back to VID:PID. A configured VID:PID never falls back to an unrelated system adapter. If several devices match, the saved matching instance is used; otherwise select one by BUSID. Bluetooth must be enabled; setting a default VID:PID alone does not attach a device.

**MediaTek limitation in the tested kernel:** the built-in MT7922 (`0489:e0e2`) did not initialize because the stock WSL `6.18.54.1` configuration has `CONFIG_BT_HCIBTUSB_MTK` disabled. Installing firmware or seeing the adapter in `lsusb` does not add the missing kernel driver support. Switching the Ubuntu distribution to 26.04 inside WSL does not enable it either. See the [configuration for that kernel version](https://github.com/microsoft/WSL2-Linux-Kernel/blob/linux-msft-wsl-6.18.54.1/arch/x86/configs/config-wsl). Other adapters may work if their driver and firmware are supported by the running WSL kernel; ASUS USB-BT500 is the verified option for this setup.

## 2. Configure your hardware (skip for XREAL Air 2 Ultra)

For XREAL Air 2 Ultra, the supplied preset is ready to use. Create your settings file once:

~~~powershell
if (!(Test-Path .env)) { Copy-Item .env.example .env }
~~~

For another device, copy the generic template:

~~~powershell
Copy-Item hardware\generic.env.example hardware\my-device.env
~~~

Edit `hardware/my-device.env`, then set `XR_HARDWARE_ENV=hardware/my-device.env` in `.env`.

1. Set `XR_PROFILE` to the runtime profile and `XR_TARGET_DEVICE` to its device name.
2. Set `XR_USB_IDS` to the camera/IMU VID:PID pairs. For a manual selection, run `usbipd list` and set `XR_USB_BUS_IDS` in `.env`. For multiple devices, separate the IDs with commas. `XR_USB_INSTANCE_IDS` accepts stable Windows USB instance paths instead.
3. Set Linux camera, HID, serial and input paths when needed: `XR_CAMERA_DEVICE`, `XR_HIDRAW_DEVICES`, `XR_SERIAL_DEVICES`, `XR_INPUT_DEVICES`. Enable `XR_REQUIRE_CAMERA=1` if the profile uses a camera.
4. Set `XR_DEVICE_ACCESS_SCRIPT` to the matching setup script inside the release, or leave it empty. List the required Linux modules in `XR_KERNEL_MODULES`.
5. Set `XR_WINDOW_WIDTH`, `XR_WINDOW_HEIGHT`, `XR_RENDER_WIDTH`, `XR_RENDER_HEIGHT`, `XR_DISPLAY_FREQUENCY_HZ` and the optical settings for your display.

To override saved choices, put the setting directly in `.env`. Select a frequency and `XR_DISPLAY_MODE` (`direct` or `extended_sbs`) included in the archive. For a dedicated Bluetooth adapter, set `XR_BLUETOOTH_USB_ID` to its VID:PID for automatic selection, or `XR_BLUETOOTH_BUS_ID` for an explicit port selection. Set `XR_BLUETOOTH=0` to disable a saved dongle, or `XR_GEARVR=0` to disable Gear VR. Set `XR_STEAMVR_PATH` if SteamVR is not found automatically.

## 3. Install

1. Double-click **INSTALL.cmd**, or run:

~~~powershell
.\INSTALL.cmd
~~~

For Gear VR controllers, use:

~~~powershell
.\INSTALL.cmd -GearVR
~~~

2. Approve the administrator prompt. Restart Windows and run the same command again.
3. If Ubuntu opens for the first time, create a Linux username and password, enter `exit`, then run the installation command again.
4. If asked to restart Ubuntu, save your work in it and run:

~~~powershell
wsl --terminate Ubuntu-24.04
.\INSTALL.cmd
~~~

Use the distribution name from `XR_DISTRIBUTION` if you changed it. Repeat `-GearVR` if used.

5. If driver registration was deferred, close SteamVR and run:

~~~powershell
.\DRIVER.cmd
~~~

## 4. Select the display

For **extended_sbs**:

1. Connect the glasses' video input to the Windows GPU and enable SBS on the glasses.
2. In Windows display settings, choose **Extend these displays**. Set the glasses' resolution and frequency to the values in your hardware env.
3. Disable SteamVR Direct Mode, then close SteamVR.
4. Run:

~~~powershell
.\DISPLAY.cmd
~~~

5. Select the glasses' row using Windows **Identify**. To save a fixed output name instead, set `XR_DISPLAY_NAME` in `.env` and run the command again.

For **direct**, use `Direct-Mode-Guide.bat` in the selected `drivers/<frequency>Hz-direct/xr_tracking` folder.

## 5. Start

Optional `.env` setting `XR_HMD_MAX_AGE_MS` controls the maximum source HMD pose
age (default 250 ms). Setting it to `500` is a temporary workaround for source
timestamp delays in WSL. It does not reduce sensor latency and increases the
time before a stopped source is reported as lost.

1. Connect the devices. Place IMU controllers on a flat surface and keep them still.
2. Run:

~~~powershell
.\RUN.cmd
~~~

3. Complete the startup prompts and calibration in the opened console. Keep it open and wait for SteamVR.
4. Enable the XR Gate add-on in SteamVR if prompted. Check the headset, then start your game.

For the next session, connect the devices and run **RUN.cmd** again. After changing display ports, close SteamVR and repeat **DISPLAY.cmd**.

To start the runtime without opening SteamVR:

~~~powershell
.\CONSOLE.cmd
~~~

## 6. Pair Gear VR controllers

With Bluetooth enabled, **Install / continue** installs the configured Ubuntu firmware packages and enables `xr-gate-wsl-firmware.service` before Bluetooth starts. The helper and configuration live inside Linux; boot does not depend on the Windows launcher folder. **Attach USB devices** also restores the shared firmware mount and kernel search path, so existing installations can be repaired without reinstalling the runtime. Only an unready selected dongle is reattached; other USB devices stay connected.

`btusb` comes from the WSL kernel. Firmware packages, source/shared directories and optional filenames are configurable through `XR_BLUETOOTH_FIRMWARE_PACKAGES` (default `linux-firmware,zstd`), `XR_WSL_FIRMWARE_SOURCE` (`/lib/firmware`), `XR_WSL_FIRMWARE_PATH` (`/mnt/wsl/xr-gate-firmware`) and `XR_WSL_FIRMWARE_FILES` (empty = discover Bluetooth firmware requests from the kernel log). Compressed `.zst`, `.xz` and `.gz` files are unpacked when needed. Set `XR_WSL_FIRMWARE=0` to disable automatic boot setup on the next Attach/Install. Readiness checks the HCI object belonging to the selected USB dongle, rather than accepting an unrelated adapter.

1. Enable Bluetooth with `XR_BLUETOOTH=1` in `.env`. For ASUS USB-BT500, leave `XR_BLUETOOTH_BUS_ID` empty and use `XR_BLUETOOTH_USB_ID=0b05:190e`, or set an explicit BUSID for another selection. Then attach the devices:

~~~powershell
.\USB.cmd
~~~

2. Stop tracking, put the controllers into pairing mode and click **Run > Pair Bluetooth controllers**.
3. Choose **Left controller** and **Right controller** by name. MAC addresses distinguish identical names. **Not selected** disables that side in the saved configuration. Choose at least one side and use different devices for the two hands.
4. Click **Connect & save**. Pairing, trust and connect run automatically, and MAC addresses/device fingerprints are saved to `~/xr-gate-wsl/config/override_controller.json`. Existing button mappings and settings are retained for selected sides. The previous configuration is backed up; connection failure leaves it unchanged.
5. Click **Start VR**. The supplied GearVR profile needs no separate registration afterward. **Register / train controllers** remains available for changing mappings and other profiles.

PowerShell also offers interactive selection by name:

~~~powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\XrGate-Wsl.ps1 -Action Pair
~~~

The Ubuntu release includes the Linux helper beside `start_override_controller.sh`. From an extracted Linux package:

~~~bash
python3 bin/scripts/override_controller/bluetooth_controllers.py \
  --config "$HOME/.config/xr_tracking/override_controller/default.json" \
  --template bin/override_controller/configs/empty_gearvr.json
~~~

Automatic identity assignment currently supports the `gearvr_ble` backend selected in the controller config. Other backends use registration/training. Button mappings, optics and USB identities remain configurable.

## 7. Stop or change settings

1. Close SteamVR.
2. Press **Ctrl+C** in the runtime console and wait for it to stop.
3. To return USB devices to Windows, run:

~~~powershell
.\DETACH.cmd
~~~

After changing hardware settings, attach the devices and start again. After changing the release, driver frequency or display mode in `.env`, repeat **INSTALL.cmd** and the display setup.

## 8. If startup fails

Run:

~~~powershell
.\DOCTOR.cmd
~~~

With the runtime console still open, test tracking:

~~~powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\XrGate-Wsl.ps1 -Action Test
~~~

If SteamVR reports an invalid desktop display, close SteamVR, check **Extend**, the resolution and frequency in Windows, then repeat **DISPLAY.cmd** and **RUN.cmd**.

## Device status and shutdown

`XR_DEVICE_NAME` in the selected hardware env supplies the readable name beside XR Gate. The XREAL profile uses `Device: XREAL ULTRA`. **CONNECTED** means the selected USB device is available to Windows; **SHARED** means it is attached to WSL through USB/IP; **NOT CONNECTED** means it was not found. Polling does not bind or attach USB devices. The header also links to the project repository.

Closing the launcher stops owned Linux services and verifies USB detach, including an Attach-only session. A failed detach does not skip the other owned devices. Incomplete shutdown keeps the window open for inspection and retry. The Linux supervisor owns a unique session token and verifies PID start time and boot identity before signalling processes.

## CI platform selection

**XR Gate split build → Run workflow** has independent **Build ubuntu release** and **Build windows release (includes SteamVR driver)** switches. Select either or both. Windows builds include the self-contained WPF executable and selected SteamVR variants in `xr-gate-windows-wsl-usbip.zip`. A Windows-only installer pins a previously published Ubuntu runtime/models release, rather than looking for Linux assets in its own Windows-only release. If no usable release is found or GitHub lookup is unavailable, Windows still builds with `XR_RELEASE_TAG=latest` and a warning. Before installing, publish an Ubuntu runtime/models release or select an available runtime tag in launcher Settings. Publishing uploads only the selected platforms. Push/PR Ubuntu checks keep their previous behavior.
