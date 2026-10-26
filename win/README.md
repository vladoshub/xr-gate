# XR Gate for Windows

XR Gate runs through the Windows Subsystem for Linux (WSL 2). The Windows launcher includes .NET.

[English](README.md) · [Русский](README.ru.md)

Install SteamVR, open it once and close it. Download and extract from the 'Releases' https://github.com/vladoshub/xr-gate/releases/latest the complete xr-gate-windows-wsl-usbip.zip and open XR-Gate.exe. Keep the supplied folders together.

## Start

1. **Select glasses display** — Connect the glasses. In Windows choose Extend these displays and enable the glasses’ SBS mode. In Run, click Select glasses display and choose their display number. Install stays disabled until the selected display is connected.
2. **Install** — Confirm installation of WSL, Ubuntu and XR Gate. Create the Ubuntu account if prompted (see below). Installation continues after you type `exit`. Restart only when XR Gate requests it; after reboot, reopen XR Gate to continue unfinished setup. Install becomes Update when setup is complete.
3. **Start VR** — Available after installation, display selection and device connection. USB connects automatically. Stop tracking & return USB, or close the launcher, to stop the session, request SteamVR exit and return the devices.

## Ubuntu account

Choose your own Linux username and password in the Ubuntu window. Password characters are not displayed while typing. After the Linux shell appears, type `exit`; keep XR Gate open and installation will continue automatically. If it stays paused, finish account setup, type `exit`, then click Install. No restart is needed unless explicitly requested.

## Tabs

- **Run** — Display selection, Install / Update, Start VR, Restart tracking, Stop, status and Activity Log. Restart tracking restarts XR Gate inside Ubuntu while keeping SteamVR and USB connected. Buttons include hover hints. Delete removes the saved Ubuntu distribution and all its Linux data after confirmation. Blinking buttons show the next setup step.
- **Controllers** — Gestures is the default; Bluetooth is off. No selects head tracking without hand/controller input. Fields save automatically. For Gear VR Controllers, enable Bluetooth, pair and select the left and/or right controller, then Connect & save. Calibration instructions appear once per stage in Activity Log.
- **Audio** — Enable is on by default; turn it off to disable sound and keep device choices. Start VR keeps an available saved Ubuntu output or selects the first available one. Playback only; no microphone.
- **Settings** — Hardware preset, display and launch settings. Hotkeys: F5 starts VR, F6 stops tracking and returns USB, F7 restarts tracking while keeping SteamVR running. Global hotkeys work while XR Gate is open, including when minimized; the checkbox saves automatically. Show command prompt is off by default; enable it before starting for console calibration commands.
- **Tools** — Diagnostics, driver registration and tracking checks. List USB devices stays in Settings.

## Bluetooth adapter for WSL2

Bluetooth was tested with ASUS USB-BT500. Use a separate adapter to keep Windows Bluetooth devices available. A built-in MediaTek adapter requires support in the WSL kernel; changing the Ubuntu release alone does not add it.

Hardware presets are in hardware/*.env; Advanced settings opens .env. The supplied PowerShell and Linux helpers are required by the launcher. Separate .cmd/.bat launchers are no longer included.

## Troubleshooting

- Follow [Next] in Activity Log. Use Tools → Diagnostics and Save log in Run.
- If USB is busy, close camera apps and SteamVR, then reconnect the device and retry Start VR.
- If reinstalling reports an old firmware mount, close XR Gate and Docker, save work in all WSL distributions, run `wsl --shutdown` in PowerShell, then reopen XR Gate and click Install. This stops all WSL distributions.
- If nothing helps, fully shut down the computer. Unplug the glasses and the separate Bluetooth adapter if used, wait a few seconds, reconnect them and turn the computer on.

For custom hardware, `XR_CAMERA_DEVICE` selects the primary camera; `XR_CAMERA_AUTO_SELECT=1` discovers a unique capture node using `XR_USB_IDS`. Keep automatic selection off for profiles that select their own cameras, such as dual-camera layouts.

SteamVR packages use `drivers/<XR_TARGET_DEVICE>/<Hz>Hz-<mode>/xr_tracking`. The active Hardware preset selects the device before refresh rate and display mode. The 60 Hz fallback stays within that device.
