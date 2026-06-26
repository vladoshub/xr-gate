# XR Client

`xr_backend_client.py` is the process orchestrator for the XR runtime package.

It starts display/capture services, runs the startup gate, starts tracking/runtime backends, manages optional services, handles tap controls, and provides the interactive backend control menu.

## Package entrypoint

```bash
cd out/xreal_ultra
./run_xr_client.sh
```

## Main config

```text
devices/xreal_ultra/configs/xr_client/default_shm.json
```

## Common manual controls

```text
1 - restart running backends
2 - start/stop hand_tracking
3 - toggle 3DoF/6DoF
4 - recenter 3DoF
5 - start/stop override_controller
6 - start/stop xr_video
7 - start/stop xr_spatial
```

Detailed config documentation is maintained separately in `xr_client_default_shm_config_readme.md`.

## Windows native profile

The Linux SHM profile remains the default on Linux. Native Windows support is kept in a separate TCP/UDP profile so it does not affect the current Linux path.

```powershell
cd C:\src\xr_tracking
powershell -ExecutionPolicy Bypass -File .\xr_client\scripts\windows\run_xr_client.ps1 -Root C:\src\xr_tracking
```

The Windows profile uses:

```text
capture_service_cpp -> TCP capture_client transport on 127.0.0.1:45660
xr_startup_gate     -> TCP capture stream quality gate
xr_runtime_adapter  -> UDP runtime output path for OpenVR driver
override_controller -> optional TCP controller_input path
```

Windows config file:

```text
xr_client/configs/default_windows_tcp.json
```

Linux still resolves `{scripts}` to `devices/xreal_ultra/linux/scripts`. Windows resolves `{scripts}` to `devices/xreal_ultra/windows/scripts` when `XR_DEVICE_SCRIPTS_OS=windows` or when running on native Windows.
