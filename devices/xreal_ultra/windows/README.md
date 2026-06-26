# XREAL Ultra Windows package scripts

This folder contains Windows-native wrappers for the partial native pipeline:

- `capture_service_cpp` runs natively on Windows and publishes TCP only.
- `xreal_display_helper` runs natively on Windows and switches XREAL display mode.
- `drivers/openvr_driver` is built/registered natively for SteamVR.
- `override_controller` can run natively on Windows and publish controller input over TCP.
- Linux backends and UDP bridge are prepared separately inside WSL2 Ubuntu 24.04.


Windows package output
----------------------

Native Windows artifacts are installed under:

```text
<root>\out\xreal_ultra_windows\bin
```

This intentionally does not reuse Linux package output `out/xreal_ultra`, so Linux and Windows deploy trees can coexist in the same checkout.

Build native Windows artifacts (`capture_service_cpp`, `xreal_display_helper`, `override_controller`, `openvr_driver`):

```powershell
powershell -ExecutionPolicy Bypass -File .\devices\xreal_ultra\windows\scripts\install_xreal_ultra_out.ps1 `
  -Root C:\src\xr_tracking `
  -VcpkgToolchain C:\vcpkg\scripts\buildsystems\vcpkg.cmake `
  -OpenCvDir C:\opencv\build `
  -OpenVrSdkRoot C:\src\openvr
```


Build or rebuild only override controller:

```powershell
powershell -ExecutionPolicy Bypass -File .\devices\xreal_ultra\windows\scripts\install_xreal_ultra_out.ps1 `
  -Root C:\src\xr_tracking `
  -BuildOnly override_controller `
  -VcpkgToolchain C:\vcpkg\scripts\buildsystems\vcpkg.cmake
```

Start native Windows override controller TCP publisher:

```powershell
powershell -ExecutionPolicy Bypass -File .\devices\xreal_ultra\windows\scripts\override_controller\start_override_controller_tcp.ps1 `
  -Root C:\src\xr_tracking
```

Register the OpenVR package selected for the current display mode:

```powershell
powershell -ExecutionPolicy Bypass -File .\devices\xreal_ultra\windows\scripts\openvr_driver\register_driver.ps1 `
  -Root C:\src\xr_tracking `
  -FrequencyHz 90
```

Prepare WSL2 runtime folders/dependencies without building binaries:

```powershell
powershell -ExecutionPolicy Bypass -File .\devices\xreal_ultra\windows\scripts\wsl2\install_wsl2_ubuntu24_runtime.ps1 `
  -Root C:\src\xr_tracking `
  -Distro Ubuntu-24.04
```

Windows + WSL2 runtime sync
---------------------------

Network/port defaults for Windows host services and WSL2 apps live in:

```text
devices/xreal_ultra/windows/configs/windows_wsl2_network.env
```

Prepare or refresh the WSL2 runtime by copying a prebuilt Linux runtime package
from Windows into the WSL2 checkout.  For example, if Linux binaries were copied
to `C:\src\xr_tracking\out\xreal_ultra`, this mirrors the contents into
`~/src/xr_tracking/out/xreal_ultra` inside WSL2:

```powershell
powershell -ExecutionPolicy Bypass -File .\devices\xreal_ultra\windows\scripts\wsl2\install_wsl2_ubuntu24_runtime.ps1 `
  -Root C:\src\xr_tracking `
  -RuntimeSourceRoot C:\src\xr_tracking\out\xreal_ultra `
  -Distro Ubuntu-24.04
```

Start the copied WSL2 runtime package manually:

```powershell
powershell -ExecutionPolicy Bypass -File .\devices\xreal_ultra\windows\scripts\wsl2\start_wsl2_runtime.ps1 `
  -Root C:\src\xr_tracking `
  -Distro Ubuntu-24.04
```

Or let `xr_client` start it by setting:

```powershell
$env:RUN_WSL2_RUNTIME="1"
```
