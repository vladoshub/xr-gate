# XR Gate

Experimental XR pipeline for **AR glasses**, designed both for low-level AR/XR research and for launching real VR/XR games and applications with spatial tracking and controller input.

XR Tracking turns XREAL Ultra into a hackable runtime platform: it captures stereo grayscale video and IMU data, synchronizes the streams, runs 6DoF/3DoF tracking and perception backends, performs ML-based hand tracking, emulates VR controllers from hand poses and physical input devices, and exposes the result to SteamVR/OpenVR, Monado/OpenXR, and standalone tools.

The goal is to keep the project useful in two directions at once:

- **For users and XR hobbyists:** build one runtime package, register the driver, start `xr_client`, and experiment with XREAL Ultra in SteamVR/OpenVR without manually wiring every backend.
- **For engineers and researchers:** use the codebase as a real-time computer vision / perception / XR systems project with C++ capture, Python stream clients, ONNX hand-tracking integration, VIO, runtime filtering, spatial mapping, IPC transports, controller emulation, and reproducible packaging.

**FOR INSTALL AND USE GO TO "Quick start"**


## What it does

The default XREAL Ultra pipeline is:

```text
XREAL Ultra
  stereo grayscale cameras + IMU
        ↓
capture_service_cpp
  low-latency C++ camera/IMU capture
        ↓
SHM / TCP streams
        ↓
capture_client + backends
  Python stream SDK, 3DoF/6DoF, hand tracking, stereo video, spatial mesh/scanner
        ↓
xr_runtime_adapter
  runtime-rate normalization, prediction, coordinate transforms, controller state
        ↓
drivers and apps
  SteamVR/OpenVR, Monado/OpenXR, debug viewers, optional overlays
```

Current focus areas:

- XREAL Ultra capture and display/runtime integration.
- 6DoF VIO through Basalt.
- IMU-only 3DoF fallback and recentering.
- Mercury/ONNX hand-tracking runtime integration.
- Physical controller override input for SteamVR-style interactions.
- Stereo video streaming and optional SteamVR video overlay experiments.
- Live depth grid / spatial proxy mesh / primitive scan output.
- Portable Linux runtime package under `out/xreal_ultra`.

## Why this project is interesting

For **Computer Vision / Perception / Robotics / Edge AI** work, this repository is more than a demo script. It contains a full real-time perception pipeline around actual hardware:

- synchronized stereo camera + IMU capture;
- stream contracts and shared-memory/TCP transports;
- Kalibr-style dataset recording and calibration tooling;
- VIO backend integration;
- ONNX hand-tracking model integration;
- runtime pose filtering, reacquisition gating, and prediction;
- live spatial grid / mesh publishing;
- debug viewers and health checks for stream timing and image quality.

For **ML / real-time inference / ML infrastructure** work, it demonstrates how models fit into a deployable runtime rather than an isolated notebook:

- explicit model download step instead of vendoring model binaries;
- runtime model directory under `bin/hand-tracking-models/mercury`;
- process orchestration through `xr_client`;
- reproducible build/package scripts;
- separation between project-owned code, third-party code, optional GPL tools, and downloaded model assets.

For **Python/C++ / AR/VR/XR systems** work, it shows cross-language runtime engineering:

- C++ backends for capture, runtime adaptation, drivers, and low-latency paths;
- Python SDK and tools for stream reading, recording, calibration, and debug workflows;
- OpenVR/SteamVR and Monado/OpenXR integration points;
- device-specific package scripts with a portable `out/xreal_ultra` layout.

## Quick start — XREAL Ultra with SteamVR/OpenVR on Ubuntu 24.04

Steam must be installed from the Steam website, not from Snap. Use an X11/Xorg session.

Build/runtime scripts may add the user to `video`, `input`, and `plugdev` groups. After that, log out/log in or reboot.

### 1. Use the release artifacts (once)

Download these artifacts:

1. xreal-ultra-linux-x64.zip

2. hand-tracking-models-mercury.zip

3. unpack-xreal-ultra.zip

Mercury ONNX models are distributed separately in hand-tracking-models-mercury.zip. They are not included in the core runtime package and are not downloaded by the default build.

Extract unpack_xreal_ultra.sh from unpack-xreal-ultra.zip and place it next to:
xreal-ultra-linux-x64.zip
hand-tracking-models-mercury.zip
Unpack the runtime package and install the Mercury models:

```bash
chmod +x unpack_xreal_ultra.sh
./unpack_xreal_ultra.sh --dest ~/xr-gate-release
```


After extraction, the runtime package will be available under:

~/xr-gate-release/xreal_ultra/...


### 2. Install runtime dependencies (once)

```bash
cd ~/xr-gate-release/xreal_ultra
./devices/xreal_ultra/linux/scripts/install_runtime_deps_ubuntu24.sh
```
After install need reboot!


### 3. Register OpenVR driver (once)

For direct USB4/iGPU path, tested with HX370 iGPU:

```bash
cd ~/xr-gate-release/xreal_ultra
XR_TARGET_DEVICE=xreal_ultra \
XR_DISPLAY_FREQUENCY_HZ=90 \
XR_OPENVR_DISPLAY_FREQUENCY_HZ=90 \
XR_OPENVR_DISPLAY_MODE=direct \
./devices/xreal_ultra/linux/scripts/openvr_driver/register_driver.sh
```

For NVIDIA dGPU with HDMI/DisplayPort -> Type-C DP adapter, 60 Hz is the safer default:

```bash
cd ~/xr-gate-release/xreal_ultra
XR_OPENVR_REGISTER_METHOD=manual \
XR_OPENVR_RUNTIME_MODE=steamvr \
XR_TARGET_DEVICE=xreal_ultra \
XR_DISPLAY_FREQUENCY_HZ=60 \
XR_OPENVR_DISPLAY_FREQUENCY_HZ=60 \
XR_OPENVR_DISPLAY_MODE=direct \
./devices/xreal_ultra/linux/scripts/openvr_driver/register_driver.sh
```

### 4. Optional: train override controller input (once)


Without this, input will be limited to gestures (pinch, grab).

Bluetooth controllers, USB keyboards, and joysticks can be mapped to runtime controller input.

You can use 2 identical Bluetooth controllers.

```bash
cd ~/xr-gate-release/xreal_ultra
bin/scripts/override_controller/start_override_controller.sh
```


### 5. Run xr_client

**You must launch the client every time before launching SteamVR.**


At startup, you need to select 60Hz or 90Hz mode.
If you have integrated graphics, a USB-C video output, and don't have a separate graphics card, you can try 90Hz mode. Otherwise, I recommend 60Hz.

It is also recommended to not cover the cameras and have sufficient lighting for tracking to work.

```bash
cd ~/xr-gate-release/xreal_ultra
./run_xr_client.sh
```

CTRL + C - exit from xr_client


If you have completed the train override controller from the previous point, you can use the non-gesture input override. To do this, press 5 after launching.

### Start on Integrated GPU with USB-C video output without discrete GPU in system

Tested on HX370


Just start Steam and start SteamVR. After you can start SteamVR apps/games.


### Start on Discrete GPU

Tested on RTX6000 Pro Blackwell


In most cases, you will need an adapter to run the glasses with a dGPU.

Check displays:

```bash
xrandr --query | grep " connected"
```

Example output:

```text
DP-6 connected primary 7680x2160+0+1080
DP-4 connected 3840x1080+1920+0
```

Here:

```text
DP-4 = glasses (3840x1080)
DP-6 = main monitor
```

Run direct mode (tested on Nvidia):
```bash
cd ~/xr-gate-release/xreal_ultra
XR_OPENVR_DGPU_GPU_VENDOR=nvidia \
XR_OPENVR_DGPU_OUTPUT=DP-4 \
XR_OPENVR_LAUNCH_MODE=steam \
XR_OPENVR_CLEAR_LOGS=1 \
XR_DISPLAY_FREQUENCY_HZ=60 \
./run_openvr_dgpu_direct.sh 60
```


Start SteamVR. After you can start SteamVR apps/games.


After finishing the work, I recommend restoring the glasses to their normal state to avoid problems during future launches or if you have an issue with xr_client or the SteamVR driver:

```bash
cd ~/xr-gate-release/xreal_ultra

XR_STEAMVR_RESTORE_OUTPUT=DP-4 \
XR_STEAMVR_RESTORE_MAIN_OUTPUT=DP-6 \
XR_STEAMVR_RESTORE_LAYOUT=right-of \
./run_openvr_restore_desktop.sh
```


## Possible problems
1. xr_client fails frame check on startup. Solution: restart xr_client, re-insert glasses, or restart

2. Tracking isn't working, or my pose is off somewhere - most likely, 6DoF received bad frames (not enough light) and can't calculate the pose correctly. The solution is to try pressing "1" in xr_client to restart the backends - this can be done at runtime and shouldn't break the current session. This can be done several times.

3. Poor hand tracking, one hand isn't visible, etc. Try removing both hands from the cameras' field of view and then bringing them back. If that doesn't help, press "1" in xr_client to restart the backends.


## Runtime package

The portable runtime package is built under:

```text
out/xreal_ultra/
  bin/                  binaries, libraries, runtime Python, scripts, apps, drivers
  devices/xreal_ultra/  device ENV, launch wrappers, configs, calibration
  run_xr_client.sh      package entrypoint
```

Build:

```bash
devices/xreal_ultra/linux/scripts/install_xreal_ultra_out.sh
```

Or clean build:

```bash
devices/xreal_ultra/linux/scripts/run_xreal_ultra_act_build.sh --clean-artifacts
```


Run it:

```bash
cd out/xreal_ultra
./run_xr_client.sh
```

## Main components

### `capture_service_cpp`

C++ camera/IMU capture backend for XREAL Ultra. It publishes synchronized stereo and IMU streams through SHM/TCP and is the default capture path.

### `capture_client`

Standalone Python reader SDK for runtime streams. It is used by recorders, calibration tools, debug viewers, and Python-side experiments.

### `backends/`

Tracking and perception backends, including hand tracking, video, spatial mapping/scanning, 3DoF, and 6DoF integration paths.

### `xr_runtime_adapter`

Runtime normalization layer. It consumes backend streams, applies transforms/prediction/stale checks, and republishes runtime-ready HMD, controller, hand, video, and spatial data.

### `xr_client`

Process orchestrator for starting, stopping, and supervising the runtime stack.

### `drivers/` and `apps/`

SteamVR/OpenVR, Monado/OpenXR, optional xrizer integration, and experimental SteamVR applications/overlays.

## Source layout

```text
shared/include/                 shared ABI/contracts
capture_client/                 standalone Python reader SDK for SHM/TCP streams
capture_service_cpp/            C++ stereo camera + IMU capture backend
backends/                       hand-tracking/video/spatial backends/3DoF/6DoF
runtime_adapters/               runtime stream normalization
bridges/                        network/UDP bridge tools
apps/steamvr/                   optional SteamVR applications
drivers/                        OpenVR, Monado, and optional xrizer integration
override_controller/            input backend that maps physical controller devices
devices/xreal_ultra/            device-specific package config/scripts
xr_client/                      process orchestrator
readme/                         additional documentation
```

Upstream code belongs under `third_party/`. Project-owned code is stored normally. Required upstream modifications should be kept as patch files.

More documentation is in the `readme/` folder.

## Optional GPL tools

`xrizer` is an optional third-party GPL-3.0-or-later tool. It is not built by default and is not part of the core binary release.

Build it explicitly:

```bash
cd ~/src/xr_tracking
XR_BUILD_ONLY=xrizer ./devices/xreal_ultra/linux/scripts/install_xreal_ultra_out.sh
```

See `readme/xrizer.md`.

## License

Project-owned source code is licensed under the MIT License. See `LICENSE`.

Some files, directories, patches, optional tools, generated binaries, downloaded models, and third-party dependencies are governed by their own upstream licenses. Files with an explicit SPDX header or local license notice keep that license.

See `THIRD_PARTY_NOTICES.md` for the main third-party components and packaging notes.

## Third-party code and references

The project uses or can build against these third-party components:

- Basalt — 6DoF VIO backend.
- Monado — OpenXR driver/runtime integration and Mercury hand-tracking pipeline integration.
- OpenVR SDK — SteamVR/OpenVR driver integration.
- nrealAirLinuxDriver — MIT-licensed XREAL/NREAL Linux driver reference used for XREAL capture/display work.
- ar-drivers-rs — MIT-licensed AR glasses tooling reference used for XREAL display helper work.
- OpenVR-xrealAirGlassesHMD — reference for OpenVR/XREAL driver behavior and configuration ideas.
- mercury_steamvr_driver — optional source for Mercury hand-tracking ONNX models; not included in the core repository/release.
- xrizer — optional GPL-3.0-or-later OpenVR-to-OpenXR compatibility tool; not built by default.
