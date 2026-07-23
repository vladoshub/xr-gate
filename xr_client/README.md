# XR Client

`xr_backend_client.py` is the process orchestrator for the XR runtime package.
It starts capture/display services, runs the startup gate, starts tracking and
runtime backends, manages optional services, and provides manual controls.

## Package entrypoint

The Linux package is hardware-neutral. Select the runtime stack explicitly by
config name:

```bash
cd out/xr-gate
./run_xr_client.sh --config xreal_ultra
./run_xr_client.sh --config xreal_ultra --basalt-mode vo
./run_xr_client.sh --config xreal_ultra --no-imu  # compatibility alias
./run_xr_client.sh --config leap_motion_uvc_nrf54l15
```

`--config` accepts either a JSON path or a profile name resolved from
`xr_client/configs` in a source checkout and `bin/python/xr_client/configs` in a
packaged runtime. Running without a config is intentionally rejected so that no
hardware profile is selected implicitly.

`--basalt-mode vio|vo` affects only the `basalt_vio` service for the current
run. The client passes `--mode vio` or `--mode vo` to `start_basalt.sh`.
`--no-imu` remains a compatibility alias for `--basalt-mode vo`. VO mode
disables Basalt's internal IMU startup gate, but does not rewrite the selected
JSON config or disable IMU publication in `capture_service_cpp` for other
consumers.

Linux profiles currently included in the package:

```text
xr_client/configs/xreal_ultra.json
xr_client/configs/leap_motion_uvc_nrf54l15.json
```

The Leap Motion profile still uses the XREAL display device environment, then
loads `devices/leap_motion_uvc_nrf54l15/tracking.env` as the tracking-sensor
layer.

## Common manual controls

```text
1 - restart running backends
2 - start/stop hand_tracking
3 - restart Basalt and toggle VIO / stereo VO
4 - recenter 3DoF
5 - start/stop override_controller
6 - start/stop xr_video
7 - start/stop xr_spatial
8 - enable/disable IMU tap controls for this session
9 - toggle 6DoF/3DoF
```

Key `3` changes only the Basalt estimator mode. When Basalt is running, the
client stops it, updates the session-local command (`--mode vio` or
`--mode vo`), removes the stale `hmd_pose` registry entry, and starts Basalt
again.
When Basalt is stopped because 3DoF is active, key `3` only selects the mode
that will be used the next time key `9` starts Basalt.

Detailed Linux config documentation is maintained in
`configs/xr_client_default_shm_config_readme.md`.

## Windows native profile

Native Windows support uses the separate TCP/UDP profile:

```powershell
powershell -ExecutionPolicy Bypass -File .\xr_client\scripts\windows\run_xr_client.ps1 -Root C:\src\xr_tracking -Config .\xr_client\configs\default_windows_tcp.json
```

Linux runtime configs use:

```text
{common_scripts} -> devices/common/linux/scripts
{device_scripts} -> scripts of the display/device profile selected by device_env
```

Hardware-neutral launchers use `{common_scripts}`. Vendor display helpers use
`{device_scripts}` only when the selected config needs them.
