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

On Linux and WSL, keys `1` through `9` execute without Enter, as they do in
the Windows console. Text aliases still use Enter. The client restores the
terminal settings on exit and temporarily enables line editing for recovery
prompts. Background services receive closed stdin so they cannot consume menu
input; foreground services and standalone controller training keep their input.

Key `5` stops the managed override controller process group, including children
whose launcher shell has already exited. A failed stop is reported instead of
forgetting the service. After an explicit stop, controller calibration console
messages remain suppressed until the next controller start. The runtime log
still records backend diagnostics. Controller IMU messages refresh the command
prompt without printing the full menu; use `h` and Enter to display the menu.

Key `3` changes only the Basalt estimator mode. When Basalt is running, the
client stops it, updates the session-local command (`--mode vio` or
`--mode vo`), removes the stale `hmd_pose` registry entry, and starts Basalt
again.
When Basalt is stopped because 3DoF is active, key `3` only selects the mode
that will be used the next time key `9` starts Basalt.

Detailed Linux config documentation is maintained in
`configs/xr_client_default_shm_config_readme.md`.

## Camera recovery during startup

Switching the glasses to SBS may disconnect and re-enumerate their USB devices.
The capture registry is written before camera/IMU devices are opened, so the
client also waits for sustained advancing sequences on each required SHM stream
and synchronized stereo pairs. The default `stream_activity_stable_s` for
`capture_service` is `1.0` (other services default to `0.0`). A single initial
burst cannot satisfy this check. Stereo tolerance follows the gate command's
`--stereo-max-delta-ms`, or the same 1 ms default. The
`capture_service` config defaults to `stream_activity_timeout_s: 5.0`; other
services keep registration-only readiness unless this option is enabled.
VO and `no_imu` services do not wait for IMU data.

By default, `gate.restart_full_startup_on_failure` is `true`. Initial startup
uses one `gate.max_attempts` budget for helper-readiness failures, capture-open
failures, missing fresh data and failed/timed-out quality checks. Each failed
attempt stops every owned startup service, waits for the old gate process to
close, clears the configured registries, then repeats prestart, all enabled
pre-gate services and the quality check. The selected display option is retained
without asking again; `current` continues to skip the display helper.
`gate.restart_backoff_s` defaults to `2.0`, and the normal mode-readback and
`pre_capture_wait_s` checks run again on every attempt. Tracking/runtime backends
start only after startup succeeds (or the user explicitly skips a quality check).

Failed-attempt logs are preserved under
`LOG_DIR/startup_attempts/<session>/attempt-N/` before their root log files are
reused. A missing/dead capture exhausts the shared budget with an error and all
startup children stopped. After an exhausted quality-check budget, the existing
R action also repeats the full startup; Enter retains the existing skip action.

Set `gate.restart_full_startup_on_failure` to `false` for the earlier behavior:
only services listed in `gate.restart_services_on_failure` are retried, without
restarting the display helper. These service retries use the configured
`gate.restart_clean_registries` and `gate.restart_backoff_s` settings.

During the check, a capture process exit triggers recovery immediately. The
quality gate fails after two seconds without stereo data. It still uses the
full five-second window and existing thresholds to assess image quality.
Use `--quality-no-frame-timeout-s` in the gate command to adjust
the stall timeout (`0` disables it). Existing JSON profiles pick up these
defaults without edits.

For Linux XREAL prestart options with readiness patterns, a running helper is
not considered confirmation of the display mode. The client waits for an
actual `display mode after` / `alive; mode=` readback matching the selected
mode. `setting display mode` and `display mode before` never confirm readiness.
GNU `stdbuf` (from coreutils) makes the existing helper's redirected C++ output
line-buffered through its shell wrapper, so this also works with existing
binaries. Once confirmed, the configured `pre_capture_wait_s` still applies;
capture preparation completes before the quality gate timer starts. A helper
that never confirms its mode produces a readiness error instead of silently
starting capture after five seconds. Camera sync timeouts include latest frame
sequences, publication ages and timestamp differences in the startup gate log.

## Controller IMU calibration messages

Each forwarded `[imu-calibration]` progress line is followed by a yellow hint:
keep the hand and controller visible, hold your head steady, and make gentle
10-20 cm forward/back or side-to-side movements with clear starts and stops.
Avoid fast wrist rotations and continue until calibration is confirmed.

A green `Left controller calibrated. IMU prediction is active.` (or right/both)
message appears when the runtime confirms yaw and translation calibration.
Legacy runtimes without translation flags use their yaw-ready flags; disabled
translation calibration does not block readiness once reported as disabled.
Once a side is confirmed, its calibration hints stop until a new alignment
status reports a reset. Trimming a service log preserves the console reader's
position within the retained tail, so old calibration messages are not replayed.
Readiness is announced again after a calibration reset and recovery. The manual
controls prompt is refreshed after each batch of forwarded calibration messages
when interactive controls are enabled. These messages use the existing
`XR_CLIENT_COLOR` setting and respect `NO_COLOR`.

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
