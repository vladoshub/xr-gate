# XR Gate for Windows

XR Gate runs through the Windows Subsystem for Linux (WSL 2). The Windows launcher includes .NET.

[English](README.md) · [Русский](README.ru.md)

Install SteamVR, open it once and close it. Extract the complete xr-gate-windows-wsl-usbip.zip and open XR-Gate.exe. Keep the supplied folders together.

## Start

1. **Select glasses display** — Connect the glasses. In Windows choose Extend these displays and enable the glasses’ SBS mode. In Run, click Select glasses display and choose their display number. Install stays disabled until the selected display is connected.
2. **Install** — Confirm installation of WSL, Ubuntu and XR Gate. Follow the Ubuntu first-login prompts if shown. Accept a restart when ready; after reboot, open XR Gate to continue unfinished setup. Install becomes Update when setup is complete.
3. **Start VR** — Available after installation, display selection and device connection. USB connects automatically. Stop tracking & return USB, or close the launcher, to stop the session and return the devices.

## Tabs

- **Run** — Display selection, Install / Update, Start VR, Stop, status and Activity Log. Blinking buttons show the next setup step.
- **Controllers** — Gestures is the default; Bluetooth is off. For Gear VR Controllers, enable Bluetooth, pair and select the left and/or right controller, then Connect & save. Calibration instructions appear once per stage in Activity Log.
- **Audio** — Choose the Windows sound source and Ubuntu output, or No audio. Choices are remembered after reconnection. Playback only; no microphone.
- **Settings** — Hardware preset, display and launch settings. Show command prompt is off by default; enable it before starting for console calibration commands.
- **Tools** — Diagnostics, driver registration and tracking checks. List USB devices stays in Settings.

## Bluetooth adapter for WSL2

Bluetooth was tested with ASUS USB-BT500. Use a separate adapter to keep Windows Bluetooth devices available. A built-in MediaTek adapter requires support in the WSL kernel; changing the Ubuntu release alone does not add it.

Hardware presets are in hardware/*.env; Advanced settings opens .env. The supplied PowerShell and Linux helpers are required by the launcher. Separate .cmd/.bat launchers are no longer included.

If startup is blocked, follow [Next] in Activity Log. For problems, use Tools → Diagnostics and Save log in Run.
