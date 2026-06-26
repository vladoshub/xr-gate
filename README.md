# XR Tracking

Real-time XR tracking stack for **XREAL Air 2 Ultra** and compatible AR/XR experiments.

It is also possible to use other XR-Glasses (SLAM + IMU) at the architectural level. But most likely, calibration for a specific model will be required + adding support for reading raw data

For install and use go to "Quick start"

XR Tracking turns AR glasses into a hackable runtime platform: it captures stereo grayscale video and IMU data, synchronizes the streams, runs tracking and perception backends, normalizes runtime poses, and exposes the result to SteamVR/OpenVR, Monado/OpenXR, debug viewers, and standalone tools.

The project is intended to be useful in two ways:

- **For users and XR hobbyists:** build one runtime package, register the driver, start `xr_client`, and experiment with XREAL Ultra in SteamVR/OpenVR.
- **For engineers and researchers:** use the codebase as a real-time computer vision / perception / XR systems project with C++ capture, Python stream clients, ONNX hand-tracking integration, VIO, runtime filtering, spatial mapping, IPC transports, and reproducible packaging.

## What it does

The default XREAL Ultra pipeline is:

```text
XREAL Air 2 Ultra
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

- XREAL Air 2 Ultra capture and display/runtime integration.
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

### 1. Install runtime dependencies

```bash
cd ~/src/xr_tracking
./devices/xreal_ultra/linux/scripts/install_runtime_deps_ubuntu24.sh
```

### 2. Build or unpack the runtime package

Build from source:

```bash
cd ~/src/xr_tracking
./devices/xreal_ultra/linux/scripts/install_xreal_ultra_out.sh
```

Or copy a release package to:

```text
~/src/xr_tracking/out/xreal_ultra
```

### 3. Optional: download Mercury hand-tracking models

Mercury ONNX models are not included in the core repository/release and are not downloaded by the default build.

Download them explicitly when hand tracking is needed:

```bash
cd ~/src/xr_tracking/out/xreal_ultra
./download_mercury_models.sh
```

The models are placed under:

```text
out/xreal_ultra/bin/hand-tracking-models/mercury/
```

### 4. Register OpenVR driver

For direct USB4/iGPU path, tested with HX370 iGPU:

```bash
cd ~/src/xr_tracking
XR_TARGET_DEVICE=xreal_ultra \
XR_DISPLAY_FREQUENCY_HZ=90 \
XR_OPENVR_DISPLAY_FREQUENCY_HZ=90 \
XR_OPENVR_DISPLAY_MODE=direct \
./devices/xreal_ultra/linux/scripts/openvr_driver/register_driver.sh
```

For NVIDIA dGPU with HDMI/DisplayPort -> Type-C DP adapter, 60 Hz is the safer default:

```bash
cd ~/src/xr_tracking
XR_OPENVR_REGISTER_METHOD=manual \
XR_OPENVR_RUNTIME_MODE=steamvr \
XR_TARGET_DEVICE=xreal_ultra \
XR_DISPLAY_FREQUENCY_HZ=60 \
XR_OPENVR_DISPLAY_FREQUENCY_HZ=60 \
XR_OPENVR_DISPLAY_MODE=direct \
./devices/xreal_ultra/linux/scripts/openvr_driver/register_driver.sh
```

### 5. Optional: train override controller input

Bluetooth controllers, USB keyboards, and joysticks can be mapped to runtime controller input.

```bash
cd ~/src/xr_tracking
REL_BUTTON_HOLD_MS=1000 \
BUTTON_HOLD_MS=160 \
REL_AXIS_HOLD_MS=180 \
GRAB_DEVICES=1 \
out/xreal_ultra/bin/scripts/override_controller/start_override_controller.sh
```

For controller-only input, set this in the runtime adapter start script/config:

```bash
CONTROLLER_INPUT_MODE="${CONTROLLER_INPUT_MODE:-controller_buttons_only}"
```

### 6. Run xr_client

```bash
cd ~/src/xr_tracking/out/xreal_ultra
./run_xr_client.sh
```

## Runtime package

The portable runtime package is built under:

```text
out/xreal_ultra/
  bin/                  binaries, libraries, runtime Python, scripts, apps, drivers
  devices/xreal_ultra/  device ENV, launch wrappers, configs, calibration
  run_xr_client.sh      package entrypoint
```

Build the package:

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
