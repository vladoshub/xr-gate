# XR Client Configuration Guide

This document explains how to configure `default_shm.json` for `xr_client`.

The file controls the complete startup and runtime orchestration flow for the XREAL/XR tracking stack:

```text
display helper
capture_service
startup gate
Basalt 6DoF
IMU 3DoF
Mercury hand tracking
xr_runtime_adapter
override_controller
xr_video
xr_spatial
runtime debug viewer
IMU tap controls
manual backend controls
```

The config is usually stored here in the portable package:

```text
devices/xreal_ultra/configs/xr_client/default_shm.json
```

In the development tree it may also exist here:

```text
xr_client/configs/default_shm.json
```

The normal package-mode launch is:

```bash
cd ~/src/xr_tracking/out/xreal_ultra
./run_xr_client.sh
```

or directly:

```bash
cd ~/src/xr_tracking/out/xreal_ultra

PYTHONPATH="$PWD/bin/python" \
python3 bin/python/xr_client/xr_backend_client.py \
  --config devices/xreal_ultra/configs/xr_client/default_shm.json
```

---

## 1. Purpose of `default_shm.json`

`default_shm.json` is the orchestration config for `xr_backend_client.py`.

It answers these questions:

```text
Which services exist?
Which services start automatically?
Which services are optional/manual?
Which command starts each service?
Which SHM streams prove that a service is ready?
Which services restart after a crash?
Which registries should be cleaned on startup?
Which tap gestures trigger runtime actions?
Which manual menu actions are available?
Which device ENV file should be loaded?
```

It does not implement tracking itself. It launches and supervises services.

---

## 2. High-level startup flow

The usual flow is:

```text
1. Load config.
2. Load device_env.
3. Resolve placeholders such as {root}, {bin}, {scripts}, {configs}, {python}.
4. Optionally clean SHM registries.
5. Run prestart control: display helper 60Hz or 90Hz.
6. Start pre-gate services:
   - capture_service
7. Wait for capture streams:
   - camera0
   - camera1
   - imu0
8. Run startup gate:
   - visual camera quality gate
   - IMU stability gate
9. Start post-gate services:
   - basalt_vio
   - mercury_hand_tracking
   - xr_runtime_adapter
   - optional/manual services
10. Start IMU tap controls.
11. Show interactive backend control menu.
12. Monitor/restart selected services.
```

---

## 3. Top-level fields

Example:

```json
{
  "root_project": "~/src/xr_tracking",
  "log_dir": "/tmp/xr_backend_client_logs",
  "clean_registries": true,
  "registries_to_clean": [
    "/tmp/capture_service_streams.json",
    "/tmp/tracking_streams.json",
    "/tmp/runtime_tracking_streams.json"
  ],
  "wait_timeout_s": 30,
  "log_max_bytes": 1048576,
  "log_trim_interval_s": 2,
  "device_env": "{root}/devices/xreal_ultra/xreal_ultra.env"
}
```

---

## 3.1 `root_project`

```json
"root_project": "~/src/xr_tracking"
```

Logical project root.

In source-tree development mode this points to the repo root:

```text
~/src/xr_tracking
```

In package mode the launcher may run from:

```text
~/src/xr_tracking/out/xreal_ultra
```

The final path resolution also depends on `device_env`.

Use this field as the base for `{root}` placeholder expansion.

---

## 3.2 `device_env`

```json
"device_env": "{root}/devices/xreal_ultra/xreal_ultra.env"
```

Shell-style ENV file loaded by `xr_client`.

This is the main bridge between `default_shm.json` and the portable package layout.

It should define or help derive:

```text
XR_PACKAGE_ROOT
XR_ROOT_PROJECT
XR_BIN_ROOT
XR_DEVICE_HOME
XR_DEVICE_SCRIPTS_ROOT
XR_DEVICE_CONFIGS_ROOT
XR_CALIB_DIR
```

In package mode these should resolve to:

```text
XR_PACKAGE_ROOT=/home/vlados/src/xr_tracking/out/xreal_ultra
XR_BIN_ROOT=/home/vlados/src/xr_tracking/out/xreal_ultra/bin
XR_DEVICE_HOME=/home/vlados/src/xr_tracking/out/xreal_ultra/devices/xreal_ultra
XR_DEVICE_SCRIPTS_ROOT=/home/vlados/src/xr_tracking/out/xreal_ultra/devices/xreal_ultra/linux/scripts
XR_DEVICE_CONFIGS_ROOT=/home/vlados/src/xr_tracking/out/xreal_ultra/devices/xreal_ultra/configs
```

Recommended rule:

```text
All device-specific paths should come from device_env.
Do not hardcode machine-specific paths in default_shm.json unless unavoidable.
```

---

## 3.3 `log_dir`

```json
"log_dir": "/tmp/xr_backend_client_logs"
```

Directory where `xr_client` writes service logs.

Each launched service usually gets its own log file.

Recommended:

```text
/tmp/xr_backend_client_logs
```

because logs are runtime-only and should not pollute the package.

---

## 3.4 `clean_registries`

```json
"clean_registries": true
```

If `true`, `xr_client` cleans selected SHM registry files before startup.

This prevents stale streams from a previous run.

Usually keep:

```json
"clean_registries": true
```

Disable only for debugging startup attach behavior.

---

## 3.5 `registries_to_clean`

```json
"registries_to_clean": [
  "/tmp/capture_service_streams.json",
  "/tmp/tracking_streams.json",
  "/tmp/runtime_tracking_streams.json"
]
```

Registry files to delete/clean at startup.

Current registry roles:

```text
/tmp/capture_service_streams.json
  camera/IMU streams from capture_service.

/tmp/tracking_streams.json
  tracking backend streams:
    hmd_pose
    hmd_pose_3dof
    hand_tracking
    controller_input

/tmp/runtime_tracking_streams.json
  runtime adapter outputs and runtime-side streams:
    runtime_hmd_pose
    runtime_hand_tracking
    runtime_controller_state
    spatial_proxy_mesh
    runtime_spatial_proxy_mesh
    runtime_spatial_summary
```

Do not remove a registry from this list unless you intentionally want to preserve stale streams across restarts.

---

## 3.6 `wait_timeout_s`

```json
"wait_timeout_s": 30
```

Default wait timeout for service readiness checks.

A service-specific timeout may override this.

Recommended:

```json
"wait_timeout_s": 30
```

Increase if startup is slow on a weaker machine.

---

## 3.7 `log_max_bytes` and `log_trim_interval_s`

```json
"log_max_bytes": 1048576,
"log_trim_interval_s": 2
```

Controls log trimming.

Meaning:

```text
log_max_bytes:
  maximum log size per service before trimming.

log_trim_interval_s:
  how often the log trimmer runs.
```

Default is approximately 1 MB per service log.

Increase `log_max_bytes` when debugging crashes or long startup sequences.

---

## 4. Placeholder expansion

Commands use placeholders:

```text
{root}
{bin}
{device}
{scripts}
{configs}
{python}
```

Expected meaning:

```text
{root}
  root project or package root.

{bin}
  binary/runtime root.
  In package mode: out/xreal_ultra/bin

{device}
  device profile root.
  In package mode: out/xreal_ultra/devices/xreal_ultra

{scripts}
  device runtime wrapper scripts.
  In package mode: out/xreal_ultra/devices/xreal_ultra/linux/scripts

{configs}
  device configs.
  In package mode: out/xreal_ultra/devices/xreal_ultra/configs

{python}
  Python interpreter used by xr_client.
```

Example:

```json
"command": [
  "{scripts}/capture_service/start_capture_service.sh"
]
```

In package mode this should resolve to something like:

```text
/home/vlados/src/xr_tracking/out/xreal_ultra/devices/xreal_ultra/linux/scripts/capture_service/start_capture_service.sh
```

Recommended rule:

```text
Use {scripts} for service start scripts.
Use {configs} for device configs/calibration.
Use {bin} for direct runtime executables or runtime Python files.
Avoid {root}/backends/... in runtime configs.
```

---

## 5. Service object model

Most service entries follow this shape:

```json
{
  "name": "service_name",
  "enable_env": "RUN_SERVICE",
  "enabled": true,
  "start_on_launch": false,
  "optional": true,
  "command": [
    "{scripts}/service/start_service.sh"
  ],
  "env": {
    "KEY": "VALUE"
  },
  "wait_streams": [
    {
      "registry": "/tmp/tracking_streams.json",
      "stream": "some_stream"
    }
  ],
  "ready_message": "service is running.",
  "start_delay_s": 0.2,
  "stop_timeout_s": 1.0,
  "restart_on_exit": true,
  "restart_on_error_only": true,
  "restart_max_attempts": 3,
  "restart_window_s": 60,
  "restart_backoff_s": 1.0,
  "restart_wait_streams": true,
  "restart_run_gate": false
}
```

---

## 5.1 `name`

```json
"name": "basalt_vio"
```

Internal service name.

This is referenced by:

```text
manual actions
tap actions
restart actions
toggle actions
logs
status messages
```

Do not rename a service unless all actions referencing it are updated.

---

## 5.2 `enable_env`

```json
"enable_env": "RUN_BASALT"
```

Environment variable used to enable/disable the service without editing JSON.

Typical behavior:

```text
RUN_BASALT=0
  disable basalt_vio

RUN_BASALT=1
  enable basalt_vio
```

This is useful for temporary testing.

---

## 5.3 `enabled`

```json
"enabled": true
```

Static config-level enable flag.

If `false`, the service is disabled unless the code explicitly supports overriding it through `enable_env`.

Recommended pattern:

```text
Core services:
  enabled=true

Optional manual services:
  enabled=true
  start_on_launch=false
  optional=true
```

---

## 5.4 `start_on_launch`

```json
"start_on_launch": false
```

Controls whether an enabled service starts automatically during normal startup.

If omitted, services are usually treated as start-on-launch.

Examples:

```text
basalt_vio:
  enabled=true
  starts automatically

imu_3dof_backend:
  enabled=true
  start_on_launch=false
  available for manual/tap toggle but not started immediately

xr_video:
  enabled=true
  start_on_launch=false
  manual key 6 starts/stops it

xr_spatial:
  enabled=true
  start_on_launch=false
  manual key 7 starts/stops it
```

---

## 5.5 `optional`

```json
"optional": true
```

Marks the service as non-critical.

If an optional service is disabled or not started, the main pipeline can still run.

Recommended for:

```text
override_controller
xr_video
xr_spatial
runtime_debug_viewer
```

Do not mark core startup services as optional unless you want the pipeline to continue without them.

---

## 5.6 `command`

```json
"command": [
  "{scripts}/basalt_vio/start_basalt.sh"
]
```

Command used to start the service.

It is an array, not a shell string.

Use one argument per array element:

```json
"command": [
  "{python}",
  "{bin}/python/xr_client/tools/xr_startup_gate.py",
  "--transport",
  "shm"
]
```

Recommended:

```text
Use wrapper scripts for services.
Keep service-specific environment in device env or the service entry's env block.
```

---

## 5.7 `env`

```json
"env": {
  "HMD_3DOF_PRIORITY": "1",
  "HMD_3DOF_STREAM": "hmd_pose_3dof",
  "HMD_3DOF_REGISTRY": "/tmp/tracking_streams.json"
}
```

Per-service environment overrides.

These values are applied only to that service process.

Use this for small service-specific overrides.

For global device paths, prefer `devices/xreal_ultra/xreal_ultra.env`.

---

## 5.8 `wait_streams`

```json
"wait_streams": [
  {
    "registry": "/tmp/tracking_streams.json",
    "stream": "hmd_pose"
  }
]
```

Readiness check based on SHM registry streams.

The service is considered ready when the listed streams appear.

Examples:

```text
capture_service:
  /tmp/capture_service_streams.json:camera0
  /tmp/capture_service_streams.json:camera1
  /tmp/capture_service_streams.json:imu0

basalt_vio:
  /tmp/tracking_streams.json:hmd_pose

imu_3dof_backend:
  /tmp/tracking_streams.json:hmd_pose_3dof

mercury_hand_tracking:
  /tmp/tracking_streams.json:hand_tracking

xr_runtime_adapter:
  /tmp/runtime_tracking_streams.json:runtime_hmd_pose
  /tmp/runtime_tracking_streams.json:runtime_hand_tracking

override_controller:
  /tmp/tracking_streams.json:controller_input

xr_spatial:
  /tmp/runtime_tracking_streams.json:spatial_proxy_mesh
```

If a wait stream is wrong, `xr_client` may think the service never became ready.

---

## 5.9 `ready_message`

```json
"ready_message": "xr_runtime is running..."
```

Message printed after service readiness is confirmed.

Use it to show operator instructions.

---

## 5.10 `start_delay_s`

```json
"start_delay_s": 0.2
```

Small delay after starting a service.

Useful for services that need a short startup settle time before readiness checks.

---

## 5.11 `stop_timeout_s`

```json
"stop_timeout_s": 1.0
```

How long to wait for graceful process exit before force stopping.

For services that need cleanup time, increase this value.

---

## 5.12 Restart policy

Example:

```json
"restart_on_exit": true,
"restart_on_error_only": true,
"restart_max_attempts": 3,
"restart_window_s": 60,
"restart_backoff_s": 2.0,
"restart_wait_streams": true,
"restart_run_gate": false
```

Meaning:

```text
restart_on_exit:
  xr_client may restart the service if it exits.

restart_on_error_only:
  restart only on non-zero/error exit.

restart_max_attempts:
  maximum restarts within restart_window_s.

restart_window_s:
  rolling time window for restart_max_attempts.

restart_backoff_s:
  delay before restarting.

restart_wait_streams:
  after restart, wait for wait_streams again.

restart_run_gate:
  rerun startup gate before restart.
```

Recommended:

```text
Basalt:
  restart_on_exit=true
  restart_on_error_only=true
  restart_max_attempts=3
  restart_backoff_s=2

Hand tracking:
  restart_on_exit=true
  restart_on_error_only=true

Optional services:
  restart_on_exit=true can be useful,
  but keep restart_max_attempts limited.
```

---

## 6. `prestart_control`

The `prestart_control` block runs before `capture_service`.

It is used to put the XREAL display into the desired mode.

Example options:

```text
1 - 60Hz high-refresh SBS/3D
2 - 90Hz high-refresh SBS/3D
```

Important fields:

```json
"prestart_control": {
  "enabled": true,
  "enable_env": "RUN_PRESTART_CONTROL",
  "prompt": true,
  "selected_option": "prompt",
  "default_option": "60hz",
  "pre_capture_wait_s": 5.0
}
```

---

## 6.1 `enabled`

```json
"enabled": true
```

Enables prestart display control.

Usually keep enabled for XREAL glasses.

---

## 6.2 `prompt`

```json
"prompt": true
```

If enabled, the user is asked to pick 60Hz or 90Hz.

Disable for unattended startup:

```json
"prompt": false
```

Then use:

```json
"selected_option": "60hz"
```

or:

```json
"selected_option": "90hz"
```

---

## 6.3 `selected_option`

```json
"selected_option": "prompt"
```

Allowed usage:

```text
prompt
  ask interactively

60hz
  start 60Hz helper automatically

90hz
  start 90Hz helper automatically
```

---

## 6.4 `default_option`

```json
"default_option": "60hz"
```

Option used when the user presses Enter or no explicit choice is made.

---

## 6.5 `pre_capture_wait_s`

```json
"pre_capture_wait_s": 5.0
```

Wait time after display helper before starting capture.

Useful because display mode switching and sensor wake-up may need a few seconds.

---

## 6.6 `options`

Each option describes one display helper.

Example:

```json
{
  "id": "60hz",
  "choice": "1",
  "label": "60Hz high-refresh SBS/3D",
  "description": "start display helper",
  "service_name": "xreal_display_helper",
  "command": [
    "{scripts}/xreal_display_helper/start_xreal_helper_60hz.sh"
  ],
  "wait_log_any": [
    "display mode after:  3",
    "display mode after: 3",
    "alive; mode=3",
    "sbs/3d 60Hz"
  ],
  "wait_timeout_s": 20.0,
  "readiness_min_alive_s": 5.0,
  "readiness_status_interval_s": 1.0,
  "stop_timeout_s": 1.0
}
```

Fields:

```text
id:
  option identifier.

choice:
  keyboard choice shown to the user.

label:
  menu label.

description:
  short explanation.

service_name:
  logical service name for logs.

command:
  helper script.

wait_log_any:
  any log line matching one of these confirms the helper reached expected mode.

wait_timeout_s:
  max wait time for expected log.

readiness_min_alive_s:
  minimum time the helper must stay alive.

readiness_status_interval_s:
  progress print interval.

stop_timeout_s:
  graceful stop timeout.
```

---

## 7. `pre_gate_services`

These services start before the startup gate.

Current main service:

```text
capture_service
```

`capture_service` publishes:

```text
/tmp/capture_service_streams.json:camera0
/tmp/capture_service_streams.json:camera1
/tmp/capture_service_streams.json:imu0
```

The startup gate depends on these streams.

Do not move `capture_service` to `post_gate_services`, because the gate needs camera and IMU data.

---

## 8. `gate`

The `gate` block runs the startup quality check.

It checks:

```text
camera visual quality
IMU stability
```

Current command uses:

```text
xr_startup_gate.py
--transport shm
--registry /tmp/capture_service_streams.json
--cam0-stream camera0
--cam1-stream camera1
--imu-stream imu0
```

---

## 8.1 Enable/disable gate

```json
"enabled": true,
"enable_env": "STARTUP_GATE"
```

Temporarily disable with:

```bash
STARTUP_GATE=0 ./run_xr_client.sh
```

Recommended default:

```json
"enabled": true
```

because Basalt startup is sensitive to bad initial visual/IMU conditions.

---

## 8.2 Visual gate parameters

Current important parameters:

```text
--visual-good-frames 30
--min-mean 22
--min-stddev 10
--max-black-fraction 0.60
--max-white-fraction 0.15
--min-corners 260
--min-grid-cells 14
--min-laplacian-stddev 16
```

Meaning:

```text
visual-good-frames:
  number of consecutive good camera frames required.

min-mean:
  minimum image brightness.

min-stddev:
  minimum image contrast/texture.

max-black-fraction:
  reject too-dark frames.

max-white-fraction:
  reject overexposed frames.

min-corners:
  minimum feature/corner count.

min-grid-cells:
  minimum image grid coverage.

min-laplacian-stddev:
  minimum sharpness/focus/texture estimate.
```

If gate is too strict in low light, reduce:

```text
--min-mean
--min-corners
--min-laplacian-stddev
```

If Basalt starts on bad frames, increase them.

---

## 8.3 IMU gate parameters

Current important parameters:

```text
--imu-good-frames 30
--imu-min-samples 10
--imu-max-gyro-norm 0.08
--imu-max-gyro-stddev 0.04
--imu-max-accel-magnitude-error 0.75
--imu-max-accel-stddev 0.35
```

Meaning:

```text
imu-good-frames:
  number of stable windows required.

imu-min-samples:
  minimum IMU samples per check.

imu-max-gyro-norm:
  reject if headset is rotating too much.

imu-max-gyro-stddev:
  reject unstable gyro noise/motion.

imu-max-accel-magnitude-error:
  reject if acceleration magnitude differs too much from gravity.

imu-max-accel-stddev:
  reject unstable acceleration.
```

If the gate often says to keep the headset still, increase tolerances slightly.

Do not loosen them too much, because Basalt initialization becomes less stable.

---

## 8.4 Gate messages

```json
"visual_progress_template": "Startup check: camera readiness {percent:.0f}% — stay in bright light and keep the cameras uncovered.",
"visual_ready_template": "Startup check: cameras are ready. Keep your head still while headset stability is checked.",
"imu_progress_template": "Startup check: headset stability {percent:.0f}% — keep your head still.",
"imu_ready_template": "Startup check: ready."
```

These are user-facing prompts.

They can be localized or shortened without changing behavior.

---

## 8.5 Gate timeout

```json
"timeout_s": 30.0,
"timeout_status_interval_s": 1.0,
"timeout_prompt": true
```

Meaning:

```text
timeout_s:
  maximum wait time.

timeout_status_interval_s:
  status print interval.

timeout_prompt:
  ask user whether to continue if gate times out.
```

Recommended:

```json
"timeout_prompt": true
```

because sometimes it is acceptable to continue manually during debugging.

---

## 9. `post_gate_services`

These services start after capture is ready and startup gate passed.

Current services:

```text
basalt_vio
imu_3dof_backend
mercury_hand_tracking
xr_runtime_adapter
override_controller
xr_video
xr_spatial
```

---

## 9.1 `basalt_vio`

Purpose:

```text
6DoF HMD tracking from stereo camera + IMU
```

Command:

```json
"{scripts}/basalt_vio/start_basalt.sh"
```

Ready stream:

```text
/tmp/tracking_streams.json:hmd_pose
```

Recommended:

```text
enabled=true
start_on_launch implicit/true
restart_on_exit=true
restart_on_error_only=true
```

Basalt is a core service for full 6DoF tracking and scanner/world-space spatial mesh.

---

## 9.2 `imu_3dof_backend`

Purpose:

```text
3DoF fallback/alternative HMD orientation backend
```

Command:

```json
"{scripts}/imu_3dof/start_imu_3dof_backend.sh"
```

Ready stream:

```text
/tmp/tracking_streams.json:hmd_pose_3dof
```

Current behavior:

```json
"start_on_launch": false
```

This means it is available for manual/tap toggle but does not start automatically.

It is used by:

```text
left triple tap:
  toggle Basalt 6DoF / IMU 3DoF

left double tap:
  recenter 3DoF
```

---

## 9.3 `mercury_hand_tracking`

Purpose:

```text
Mercury hand tracking backend
```

Command:

```json
"{scripts}/mercury_hand_tracking/start_hand_tracking.sh"
```

Ready stream:

```text
/tmp/tracking_streams.json:hand_tracking
```

Manual/tap controls can stop/start it.

If hand tracking is unstable, tune the backend wrapper and runtime adapter transform config, not only this `xr_client` config.

---

## 9.4 `xr_runtime_adapter`

Purpose:

```text
Convert backend streams into runtime streams for OpenVR/Monado/debug viewers.
```

Command:

```json
"{scripts}/xr_runtime_adapter/start_xr_runtime_adapter_shm.sh"
```

Ready streams:

```text
/tmp/runtime_tracking_streams.json:runtime_hmd_pose
/tmp/runtime_tracking_streams.json:runtime_hand_tracking
```

Important per-service env:

```json
"env": {
  "HMD_3DOF_PRIORITY": "1",
  "HMD_3DOF_STREAM": "hmd_pose_3dof",
  "HMD_3DOF_REGISTRY": "/tmp/tracking_streams.json"
}
```

Meaning:

```text
HMD_3DOF_PRIORITY=1:
  if hmd_pose_3dof exists and is valid, runtime adapter may prefer it over 6DoF.

HMD_3DOF_STREAM:
  stream name for 3DoF pose.

HMD_3DOF_REGISTRY:
  registry file where 3DoF pose is published.
```

This is what enables runtime switching between Basalt 6DoF and IMU 3DoF.

---

## 9.5 `override_controller`

Purpose:

```text
Read physical controller/button input and publish controller_input.
```

Current behavior:

```json
"start_on_launch": false,
"optional": true
```

Manual/tap controls can start it.

Important env:

```json
"CONFIG_PATH": "~/.config/xr_tracking/override_controller/default.json",
"NON_INTERACTIVE": "1",
"GRAB_DEVICES": "1",
"REATTACH_DEVICES": "1",
"REATTACH_INTERVAL_MS": "3000"
```

Meaning:

```text
CONFIG_PATH:
  trained/selected controller mapping file.

NON_INTERACTIVE:
  do not enter interactive training.

GRAB_DEVICES:
  grab evdev devices where supported.

REATTACH_DEVICES:
  keep trying to reattach devices.

REATTACH_INTERVAL_MS:
  reattach polling interval.
```

Ready stream:

```text
/tmp/tracking_streams.json:controller_input
```

---

## 9.6 `xr_video`

Purpose:

```text
Stereo video backend.
```

Current behavior:

```json
"start_on_launch": false,
"optional": true
```

Manual key:

```text
6 - start/stop xr_video
```

No wait streams are configured:

```json
"restart_wait_streams": false
```

This is acceptable if video is optional and readiness is not critical for the main tracking pipeline.

---

## 9.7 `xr_spatial`

Purpose:

```text
Live spatial proxy mesh / organized depth grid / primitive scanner backend.
```

Current behavior:

```json
"start_on_launch": false,
"optional": true
```

Manual key:

```text
7 - start/stop xr_spatial
```

Command:

```json
"{scripts}/xr_spatial/start_xr_spatial_shm.sh"
```

Per-service env:

```json
"SPATIAL_PROXY_MESH_RATE_HZ": "10"
```

Ready stream:

```text
/tmp/runtime_tracking_streams.json:spatial_proxy_mesh
```

Important note:

```text
xr_spatial publishes spatial_proxy_mesh.
xr_runtime_adapter consumes spatial_proxy_mesh and may publish runtime_spatial_proxy_mesh.
```

For 6DoF world-space mesh, `xr_spatial` should usually run with:

```bash
SPATIAL_POSE_INPUT=shm
```

For 3DoF/no-Basalt live passthrough:

```bash
SPATIAL_POSE_INPUT=none
```

and `camera_relative_runtime.enabled=true` in the runtime adapter transform config.

---

## 10. `foreground_services`

Foreground services are launched in the foreground rather than as background-managed services.

Current service:

```text
runtime_debug_viewer
```

Example:

```json
{
  "name": "runtime_debug_viewer",
  "enable_env": "RUN_VIEWER",
  "enabled": false,
  "foreground": true,
  "command": [
    "{python}",
    "{bin}/python/tools/runtime_debug_viewer/xr_runtime_debug_viewer.py",
    "--config",
    "{configs}/runtime_debug_viewer/xr_runtime_stock.yaml"
  ]
}
```

Use for debugging.

Usually keep:

```json
"enabled": false
```

and enable manually when needed:

```bash
RUN_VIEWER=1 ./run_xr_client.sh
```

---

## 11. `imu_tap_controls`

The `imu_tap_controls` block maps physical taps on the XREAL glasses to actions.

Top-level:

```json
"imu_tap_controls": {
  "enabled": true,
  "enable_env": "RUN_IMU_TAP_CONTROLS",
  "debug": false,
  "source": {},
  "detector": {},
  "actions": {}
}
```

---

## 11.1 Enable/disable

```json
"enabled": true,
"enable_env": "RUN_IMU_TAP_CONTROLS"
```

Temporarily disable:

```bash
RUN_IMU_TAP_CONTROLS=0 ./run_xr_client.sh
```

Enable debug logs:

```json
"debug": true
```

---

## 11.2 IMU source

```json
"source": {
  "transport": "shm",
  "registry": "/tmp/capture_service_streams.json",
  "imu_stream": "imu0",
  "poll_sleep_ms": 1.0
}
```

Meaning:

```text
transport:
  currently SHM.

registry:
  capture service registry.

imu_stream:
  IMU stream name.

poll_sleep_ms:
  polling sleep interval.
```

Do not change this unless capture_service publishes IMU under a different name or registry.

---

## 11.3 Tap detector parameters

```json
"detector": {
  "tap_accel_threshold": 15.0,
  "tap_refractory_ms": 100.0,
  "impact_end_ratio": 0.7,
  "impact_max_width_ms": 60.0,
  "triple_min_interval_ms": 250.0,
  "triple_max_interval_ms": 650.0,
  "triple_max_span_ms": 1400.0,
  "cooldown_ms": 4000.0,
  "side_axis": "ax",
  "side_deadzone_mps2": 3.0,
  "double_min_interval_ms": 250.0,
  "double_max_interval_ms": 650.0,
  "double_max_span_ms": 900.0,
  "double_emit_delay_ms": 700.0,
  "triple_emit_delay_ms": 750.0,
  "quadruple_min_interval_ms": 250.0,
  "quadruple_max_interval_ms": 650.0,
  "quadruple_max_span_ms": 2100.0
}
```

Important parameters:

```text
tap_accel_threshold:
  acceleration threshold for tap detection.

tap_refractory_ms:
  minimum time between impact detections.

impact_max_width_ms:
  maximum impact duration.

double_min_interval_ms / double_max_interval_ms:
  valid interval between taps for double tap.

triple_min_interval_ms / triple_max_interval_ms:
  valid interval for triple tap.

quadruple_min_interval_ms / quadruple_max_interval_ms:
  valid interval for quadruple tap.

cooldown_ms:
  global cooldown after an action.

side_axis:
  IMU acceleration axis used to classify left/right side.

side_deadzone_mps2:
  minimum side-axis magnitude for side classification.
```

If taps are not detected:

```text
lower tap_accel_threshold slightly
increase max intervals
enable debug
```

If false positives happen:

```text
raise tap_accel_threshold
increase tap_refractory_ms
increase side_deadzone_mps2
```

---

## 12. Tap/manual actions

Actions live under:

```json
"imu_tap_controls": {
  "actions": {}
}
```

Each action has a `type` and type-specific fields.

Current important action types:

```text
recenter_3dof
toggle_service
toggle_exclusive_services
restart_services
```

---

## 12.1 `recenter_3dof`

Example:

```json
"manual-recenter-3dof": {
  "type": "recenter_3dof",
  "service": "imu_3dof_backend",
  "only_if_service_running": true,
  "control_file": "/tmp/xr_backend_control.json",
  "counter_key": "imu_3dof_recenter_counter"
}
```

Meaning:

```text
service:
  service that must be running for recenter.

only_if_service_running:
  ignore action if service is not running.

control_file:
  shared control JSON file.

counter_key:
  counter incremented to request recenter.
```

Used by:

```text
left-double-tap
manual-recenter-3dof
```

---

## 12.2 `toggle_service`

Example:

```json
"manual-toggle-hand-tracking": {
  "type": "toggle_service",
  "service": "mercury_hand_tracking",
  "wait_streams": true,
  "clean_streams_on_stop": [
    {
      "registry": "/tmp/tracking_streams.json",
      "streams": ["hand_tracking"]
    }
  ],
  "clean_streams_on_start": [
    {
      "registry": "/tmp/tracking_streams.json",
      "streams": ["hand_tracking"]
    }
  ]
}
```

Meaning:

```text
If the service is running:
  stop it and optionally clean streams.

If the service is stopped:
  clean stale streams, start it, wait for readiness if wait_streams=true.
```

Used by:

```text
left-quadruple-tap:
  mercury_hand_tracking

right-quadruple-tap:
  override_controller

manual-toggle-xr-video:
  xr_video

manual-toggle-xr-spatial:
  xr_spatial
```

---

## 12.3 `toggle_exclusive_services`

Example:

```json
"manual-toggle-3dof-6dof": {
  "type": "toggle_exclusive_services",
  "primary_service": "basalt_vio",
  "secondary_service": "imu_3dof_backend",
  "start_when_none": "imu_3dof_backend",
  "wait_streams": true
}
```

Meaning:

```text
If primary is running:
  stop primary and start secondary.

If secondary is running:
  stop secondary and start primary.

If neither is running:
  start start_when_none.
```

Used by:

```text
left-triple-tap
manual-toggle-3dof-6dof
```

Current pairing:

```text
primary_service:
  basalt_vio

secondary_service:
  imu_3dof_backend
```

This implements 6DoF/3DoF switching.

---

## 12.4 `restart_services`

Example:

```json
"manual-restart-running-backends": {
  "type": "restart_services",
  "services": [
    "basalt_vio",
    "imu_3dof_backend",
    "mercury_hand_tracking",
    "xr_spatial"
  ],
  "running_only": true,
  "run_gate": false,
  "wait_streams": true
}
```

Meaning:

```text
Restart listed services.
If running_only=true, restart only those currently running.
If wait_streams=true, wait for readiness after restart.
```

Used by:

```text
right-triple-tap
manual-restart-running-backends
```

Current restart list:

```text
basalt_vio
imu_3dof_backend
mercury_hand_tracking
xr_spatial
```

`xr_runtime_adapter` is intentionally not always in this list because it is usually the consumer/bridge that should keep running while backends restart.

---

## 13. Stream cleanup actions

Example:

```json
"clean_streams_on_stop": [
  {
    "registry": "/tmp/tracking_streams.json",
    "streams": [
      "hand_tracking"
    ]
  }
]
```

Cleanup removes stale registry entries.

Use cleanup when stopping/starting services that publish SHM streams.

Common cleanup mappings:

```text
basalt_vio:
  /tmp/tracking_streams.json:hmd_pose

imu_3dof_backend:
  /tmp/tracking_streams.json:hmd_pose_3dof

mercury_hand_tracking:
  /tmp/tracking_streams.json:hand_tracking

override_controller:
  /tmp/tracking_streams.json:controller_input

xr_spatial:
  /tmp/runtime_tracking_streams.json:spatial_proxy_mesh
  /tmp/runtime_tracking_streams.json:runtime_spatial_summary
```

Do not clean streams owned by another service unless intentional.

Example:

```text
Do not clean runtime_spatial_proxy_mesh from xr_spatial action
if it is published by xr_runtime_adapter.
```

---

## 14. Manual backend controls

The interactive menu is backed by `imu_tap_controls.actions`.

Current expected controls:

```text
1 - restart running backends
2 - start/stop hand_tracking
3 - toggle 3DoF/6DoF
4 - recenter 3DoF
5 - start/stop override_controller
6 - start/stop xr_video
7 - start/stop xr_spatial
```

Typical mapping:

```text
1:
  manual-restart-running-backends

2:
  manual-toggle-hand-tracking

3:
  manual-toggle-3dof-6dof

4:
  manual-recenter-3dof

5:
  manual-toggle-override-controller

6:
  manual-toggle-xr-video

7:
  manual-toggle-xr-spatial
```

If the menu does not show an action, check the `manual_controls.py` mapping as well as this JSON.

The JSON defines the action behavior; Python code usually defines which keyboard number maps to which action ID.

---

## 15. Current tap mapping

Current tap actions:

```text
left double tap:
  recenter 3DoF

left triple tap:
  toggle Basalt 6DoF / IMU 3DoF

left quadruple tap:
  toggle Mercury hand tracking

right triple tap:
  restart running backends

right quadruple tap:
  toggle override_controller
```

Recommended safety rules:

```text
Use double tap only for low-risk actions.
Use triple/quadruple tap for actions that stop/restart services.
Keep cooldown high enough to prevent accidental repeated restarts.
```

---

## 16. Package-mode configuration

In package mode, the important roots are:

```text
out/xreal_ultra/
  bin/
  devices/xreal_ultra/
  run_xr_client.sh
```

`default_shm.json` should use:

```text
{scripts}/...
{configs}/...
{bin}/...
```

and avoid direct source paths such as:

```text
{root}/backends/...
{root}/runtime_adapters/...
{root}/xr_client/...
```

Correct package-style examples:

```json
"command": [
  "{scripts}/capture_service/start_capture_service.sh"
]
```

```json
"command": [
  "{python}",
  "{bin}/python/xr_client/tools/xr_startup_gate.py"
]
```

```json
"command": [
  "{python}",
  "{bin}/python/tools/runtime_debug_viewer/xr_runtime_debug_viewer.py",
  "--config",
  "{configs}/runtime_debug_viewer/xr_runtime_stock.yaml"
]
```

Problematic source-tree style:

```json
"command": [
  "{root}/backends/xr_spatial/scripts/linux/start_xr_spatial_shm.sh"
]
```

This should be replaced with:

```json
"command": [
  "{scripts}/xr_spatial/start_xr_spatial_shm.sh"
]
```

---

## 17. Common configuration recipes

### 17.1 Start without Basalt by default

Set:

```json
{
  "name": "basalt_vio",
  "enabled": false
}
```

or run:

```bash
RUN_BASALT=0 ./run_xr_client.sh
```

If you still need HMD pose, start 3DoF:

```bash
RUN_IMU_3DOF_BACKEND=1 ./run_xr_client.sh
```

and make sure `imu_3dof_backend` is started manually or `start_on_launch=true`.

---

### 17.2 Start 3DoF automatically

In `imu_3dof_backend`:

```json
"start_on_launch": true
```

If Basalt is also enabled, runtime adapter priority decides which pose source is used.

For 3DoF-only mode, disable Basalt:

```bash
RUN_BASALT=0 ./run_xr_client.sh
```

---

### 17.3 Disable hand tracking

Temporary:

```bash
RUN_HAND_TRACKING=0 ./run_xr_client.sh
```

Permanent:

```json
{
  "name": "mercury_hand_tracking",
  "enabled": false
}
```

---

### 17.4 Start override controller automatically

In `override_controller`:

```json
"start_on_launch": true
```

Make sure:

```text
CONFIG_PATH
```

points to a valid trained controller mapping.

---

### 17.5 Start xr_video automatically

In `xr_video`:

```json
"start_on_launch": true
```

Keep it optional unless video is required for your runtime.

---

### 17.6 Start xr_spatial automatically

In `xr_spatial`:

```json
"start_on_launch": true
```

For live mesh at 10 Hz:

```json
"env": {
  "SPATIAL_PROXY_MESH_RATE_HZ": "10"
}
```

For 3DoF/no-Basalt passthrough, also configure the `xr_spatial` wrapper or env with:

```bash
SPATIAL_POSE_INPUT=none
```

and set `camera_relative_runtime.enabled=true` in the runtime adapter transform config.

---

### 17.7 Disable startup gate

Temporary:

```bash
STARTUP_GATE=0 ./run_xr_client.sh
```

Permanent:

```json
"gate": {
  "enabled": false
}
```

Not recommended for normal Basalt startup.

---

### 17.8 Run without prestart display prompt

Set:

```json
"prestart_control": {
  "prompt": false,
  "selected_option": "60hz"
}
```

or:

```json
"selected_option": "90hz"
```

---

### 17.9 Increase Basalt restart tolerance

In `basalt_vio`:

```json
"restart_max_attempts": 5,
"restart_window_s": 120,
"restart_backoff_s": 3.0
```

Useful if Basalt sometimes crashes during initialization.

Do not set unlimited restarts without logs, because it can hide real failures.

---

## 18. Validation

### 18.1 Validate JSON syntax

```bash
python3 -m json.tool devices/xreal_ultra/configs/xr_client/default_shm.json >/tmp/default_shm_check.json
```

or:

```bash
jq . devices/xreal_ultra/configs/xr_client/default_shm.json >/tmp/default_shm_check.json
```

---

### 18.2 Check resolved commands

From package root:

```bash
cd ~/src/xr_tracking/out/xreal_ultra

PYTHONPATH="$PWD/bin/python" python3 - <<'PY'
from xr_client.config_reader import load_config

cfg = load_config("devices/xreal_ultra/configs/xr_client/default_shm.json")

print("root:", cfg.root_project)
print("device_env:", getattr(cfg, "device_env", None))
print()

for s in cfg.pre_gate_services + cfg.post_gate_services + cfg.foreground_services:
    print(s.name)
    print("  command:", s.command)
    print("  cwd:", getattr(s, "cwd", None))
PY
```

Expected package-mode commands should resolve to:

```text
out/xreal_ultra/devices/xreal_ultra/linux/scripts/...
out/xreal_ultra/bin/...
```

They should not resolve to source-tree service paths unless you are intentionally running in dev mode.

---

### 18.3 Check service logs

Logs are under:

```text
/tmp/xr_backend_client_logs
```

Useful checks:

```bash
ls -lah /tmp/xr_backend_client_logs
tail -f /tmp/xr_backend_client_logs/*.log
```

---

### 18.4 Check registries

```bash
cat /tmp/capture_service_streams.json | jq .
cat /tmp/tracking_streams.json | jq .
cat /tmp/runtime_tracking_streams.json | jq .
```

Expected basic streams:

```text
capture_service:
  camera0
  camera1
  imu0

Basalt:
  hmd_pose

3DoF:
  hmd_pose_3dof

Hand tracking:
  hand_tracking

Runtime adapter:
  runtime_hmd_pose
  runtime_hand_tracking
  runtime_controller_state

xr_spatial:
  spatial_proxy_mesh
```

---

## 19. Troubleshooting

### capture_service does not become ready

Check:

```text
/tmp/capture_service_streams.json:camera0
/tmp/capture_service_streams.json:camera1
/tmp/capture_service_streams.json:imu0
```

Check log:

```bash
tail -f /tmp/xr_backend_client_logs/capture_service.log
```

Possible causes:

```text
XREAL glasses not awake
camera device busy
wrong calibration/profile
capture wrapper path wrong
permissions issue
```

---

### Startup gate times out on camera readiness

Improve lighting and keep cameras uncovered.

If needed, tune:

```text
--min-mean
--min-stddev
--min-corners
--min-laplacian-stddev
```

Do not loosen too much, because Basalt may initialize badly.

---

### Startup gate times out on IMU stability

Keep the glasses/headset still.

If needed, tune:

```text
--imu-max-gyro-norm
--imu-max-gyro-stddev
--imu-max-accel-stddev
```

---

### Basalt keeps restarting

Check:

```bash
tail -f /tmp/xr_backend_client_logs/basalt_vio.log
```

Common causes:

```text
bad startup frames
wrong calibration
wrong camera orientation/profile
stale pose stream
numeric failure during optimization
```

Keep `restart_max_attempts` limited so the issue remains visible.

---

### 3DoF toggle works but runtime does not switch

Check `xr_runtime_adapter` env:

```json
"HMD_3DOF_PRIORITY": "1",
"HMD_3DOF_STREAM": "hmd_pose_3dof",
"HMD_3DOF_REGISTRY": "/tmp/tracking_streams.json"
```

Check that `imu_3dof_backend` publishes:

```text
/tmp/tracking_streams.json:hmd_pose_3dof
```

---

### Hand tracking toggle does not work

Check action service name:

```json
"service": "mercury_hand_tracking"
```

must match the service entry:

```json
"name": "mercury_hand_tracking"
```

Check stream cleanup:

```text
/tmp/tracking_streams.json:hand_tracking
```

---

### xr_video starts but runtime shows no video

`xr_video` has no wait stream in this config.

Check its own registry/logs:

```text
/tmp/xr_video_streams.json
```

and runtime adapter video input settings.

---

### xr_spatial starts but runtime adapter says spatial stream not found

Check that `xr_spatial` publishes:

```text
/tmp/runtime_tracking_streams.json:spatial_proxy_mesh
```

Check service action cleanup does not remove the stream after startup.

Check logs:

```bash
tail -f /tmp/xr_backend_client_logs/xr_spatial.log
tail -f /tmp/xr_backend_client_logs/xr_runtime_adapter.log
```

---

### Manual key is shown but does nothing

Check both places:

```text
1. Python manual control mapping.
2. imu_tap_controls.actions entry in JSON.
```

The JSON defines what an action does. The Python manual controls map keyboard input to action names.

---

### Tap action triggers the wrong side

Tune:

```json
"side_axis": "ax",
"side_deadzone_mps2": 3.0
```

If left/right is reversed, this may require code-side side interpretation or changing the selected axis depending on IMU convention.

---

## 20. Safe editing rules

1. Validate JSON after every edit.

```bash
python3 -m json.tool default_shm.json >/tmp/default_shm_check.json
```

2. Change one service at a time.

3. Prefer temporary ENV overrides before permanent JSON changes.

4. Keep service names stable.

5. Use `{scripts}`, `{configs}`, and `{bin}` instead of hardcoded source-tree paths.

6. Clean only streams owned by the service being toggled.

7. Keep restart limits finite.

8. Keep core services non-optional unless deliberately testing degraded modes.

9. Keep package runtime config free from build/source paths.

10. After changing service startup order, verify readiness streams and registry ownership.

---

## 21. Quick reference

### Disable Basalt

```bash
RUN_BASALT=0 ./run_xr_client.sh
```

### Disable hand tracking

```bash
RUN_HAND_TRACKING=0 ./run_xr_client.sh
```

### Disable startup gate

```bash
STARTUP_GATE=0 ./run_xr_client.sh
```

### Disable tap controls

```bash
RUN_IMU_TAP_CONTROLS=0 ./run_xr_client.sh
```

### Enable runtime debug viewer

```bash
RUN_VIEWER=1 ./run_xr_client.sh
```

### Start 60Hz display helper without prompt

```json
"prestart_control": {
  "prompt": false,
  "selected_option": "60hz"
}
```

### Start 90Hz display helper without prompt

```json
"prestart_control": {
  "prompt": false,
  "selected_option": "90hz"
}
```

### Make xr_spatial auto-start

```json
{
  "name": "xr_spatial",
  "start_on_launch": true
}
```

### Make xr_video auto-start

```json
{
  "name": "xr_video",
  "start_on_launch": true
}
```

### Make override_controller auto-start

```json
{
  "name": "override_controller",
  "start_on_launch": true
}
```

### Manual controls

```text
1 - restart running backends
2 - start/stop hand_tracking
3 - toggle 3DoF/6DoF
4 - recenter 3DoF
5 - start/stop override_controller
6 - start/stop xr_video
7 - start/stop xr_spatial
```

### Tap controls

```text
left double tap:
  recenter 3DoF

left triple tap:
  toggle 3DoF/6DoF

left quadruple tap:
  toggle hand tracking

right triple tap:
  restart running backends

right quadruple tap:
  toggle override controller
```
