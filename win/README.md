# XR Gate on Windows (installation via WSL2)

[English](README.md) · [Русский](README.ru.md)

## 1. Download and prepare

1. Use Windows 11 x64 and an AVX2-capable CPU.
2. Install Steam and SteamVR. Open SteamVR once, then close it.
3. Download **xr-gate-windows.zip** from [Releases](https://github.com/vladoshub/xr-gate/releases), or the **xr-gate-windows** artifact from your CI run.
4. Extract the ZIP to a permanent folder. Open its `xr-gate-windows` folder. Keep the `drivers`, `hardware`, `linux` and `windows` subfolders together.
5. Open a regular Windows PowerShell in this folder. Run all commands below there.

## 2. Configure your hardware

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

To override saved choices, put the setting directly in `.env`. Select a frequency and `XR_DISPLAY_MODE` (`direct` or `extended_sbs`) included in the archive. For a dedicated Bluetooth dongle, set `XR_BLUETOOTH_BUS_ID` to its BUSID. Set `XR_BLUETOOTH=0` to disable a saved dongle, or `XR_GEARVR=0` to disable Gear VR. Set `XR_STEAMVR_PATH` if SteamVR is not found automatically.

## 3. Install

1. Double-click **INSTALL.cmd**, or run:

~~~powershell
.\INSTALL.cmd
~~~

For Gear VR controllers, use:

~~~powershell
.\INSTALL.cmd -GearVR
~~~

2. Approve the administrator prompt. If a restart is requested, restart Windows and run the same command again.
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

1. Set the dongle's `XR_BLUETOOTH_BUS_ID` in `.env`, then attach the devices:

~~~powershell
.\USB.cmd
~~~

2. Open the pairing console:

~~~powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\XrGate-Wsl.ps1 -Action Pair
~~~

3. Put the controllers into pairing mode. Replace `MAC_LEFT` and `MAC_RIGHT` with their addresses and enter:

~~~text
power on
agent on
default-agent
scan on
pair MAC_LEFT
trust MAC_LEFT
connect MAC_LEFT
pair MAC_RIGHT
trust MAC_RIGHT
connect MAC_RIGHT
scan off
quit
~~~

4. Stop the runtime if it is running. Start registration:

~~~powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\XrGate-Wsl.ps1 -Action Controllers -GearVR
~~~

5. Follow the left/right assignment prompts, save the configuration, then run **RUN.cmd**.

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
