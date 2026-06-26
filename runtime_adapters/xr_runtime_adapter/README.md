# XREAL Display Helper

Small cross-platform helper for switching XREAL Air-family glasses display modes through the MCU HID interface.

Used by `xr_client` prestart control before capture starts. On Windows it can also be launched directly before SteamVR/OpenVR startup.

## Typical modes

```text
60Hz high-refresh SBS/3D
90Hz high-refresh SBS/3D
```

## Linux package output

```text
out/xreal_ultra/bin/xreal_display_helper/
```

Normally this is launched through:

```text
devices/xreal_ultra/linux/scripts/xreal_display_helper/
```

## Windows build

Install `hidapi` through vcpkg, then build with PowerShell:

```powershell
cd C:\src\xr_tracking

powershell -ExecutionPolicy Bypass -File .\xreal_display_helper\scripts\windows\build_xreal_display_helper.ps1 `
  -Root C:\src\xr_tracking `
  -VcpkgToolchain C:\vcpkg\scripts\buildsystems\vcpkg.cmake
```

## Windows run

List HID devices first if auto interface detection does not work:

```powershell
powershell -ExecutionPolicy Bypass -File .\xreal_display_helper\scripts\windows\start_xreal_display_helper.ps1 `
  -Root C:\src\xr_tracking `
  -ListDevices
```

Switch to 90 Hz SBS/3D and keep the MCU HID handle alive:

```powershell
powershell -ExecutionPolicy Bypass -File .\xreal_display_helper\scripts\windows\start_xreal_display_helper.ps1 `
  -Root C:\src\xr_tracking `
  -Mode 90hz `
  -KeepRunning
```

If Windows hidapi reports a different interface layout, copy the exact path from `-ListDevices` and pass it through `-Path`.
