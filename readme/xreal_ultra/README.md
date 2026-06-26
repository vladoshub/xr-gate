# HOW TO RUN — XR Tracking / XREAL Ultra

## Copy `out` to target machine

Main runtime package:

```bash
out/xreal_ultra
```

Windows runtime package:

```powershell
out\xreal_ultra_windows
```

---

## License note

Project-owned code and documentation are MIT-licensed unless a file states otherwise. Optional third-party tools, downloaded models, SDKs, upstream patches, and generated binaries keep their own upstream licenses. See the root `LICENSE` and `THIRD_PARTY_NOTICES.md`.

# XREAL Ultra / Linux

## 1. Install runtime deps

Build/runtime scripts may add the user to `video`, `input`, and `plugdev` groups. After that, log out/log in or reboot.

```bash
cd ~/src/xr_tracking
./devices/xreal_ultra/linux/scripts/install_runtime_deps_ubuntu24.sh
```

---

## 2. Build package

Default full build:

```bash
cd ~/src/xr_tracking
./devices/xreal_ultra/linux/scripts/install_xreal_ultra_out.sh
```

The device target is explicit now. For XREAL Ultra it defaults to `xreal_ultra`, but you can pass it explicitly:

```bash
cd ~/src/xr_tracking
XR_TARGET_DEVICE=xreal_ultra \
XR_DEVICE_TARGET=xreal_ultra \
./devices/xreal_ultra/linux/scripts/install_xreal_ultra_out.sh
```


### Optional: download Mercury hand-tracking models

Mercury ONNX models are not included in the core repository/release and are not downloaded by the default build. Download them explicitly when needed:

```bash
cd ~/src/xr_tracking/out/xreal_ultra
./download_mercury_models.sh
```

The files are placed under:

```text
out/xreal_ultra/bin/hand-tracking-models/mercury/
```

### Partial builds

`XR_BUILD_ONLY` can be used for CI/dev builds when you do not want to rebuild everything.

Build only C++ capture service:

```bash
cd ~/src/xr_tracking
XR_BUILD_ONLY=capture_service_cpp \
XR_TARGET_DEVICE=xreal_ultra \
./devices/xreal_ultra/linux/scripts/install_xreal_ultra_out.sh
```

Build drivers only — OpenVR + Monado:

```bash
cd ~/src/xr_tracking
XR_BUILD_ONLY=drivers \
XR_TARGET_DEVICE=xreal_ultra \
./devices/xreal_ultra/linux/scripts/install_xreal_ultra_out.sh
```

Build OpenVR/SteamVR driver only:

```bash
cd ~/src/xr_tracking
XR_BUILD_ONLY=openvr_driver \
XR_TARGET_DEVICE=xreal_ultra \
./devices/xreal_ultra/linux/scripts/install_xreal_ultra_out.sh
```

Aliases:

```bash
XR_BUILD_ONLY=steamvr   # same as openvr_driver
```

Build Monado/OpenXR driver only:

```bash
cd ~/src/xr_tracking
XR_BUILD_ONLY=monado_driver \
XR_TARGET_DEVICE=xreal_ultra \
XR_MONADO_DEVICE=xreal_ultra \
./devices/xreal_ultra/linux/scripts/install_xreal_ultra_out.sh
```

Alias:

```bash
XR_BUILD_ONLY=openxr    # same as monado_driver
```

Build optional xrizer only. xrizer is GPL-3.0-or-later upstream and is not built by default:

```bash
cd ~/src/xr_tracking
XR_BUILD_ONLY=xrizer \
XR_TARGET_DEVICE=xreal_ultra \
./devices/xreal_ultra/linux/scripts/install_xreal_ultra_out.sh
```

### Build OpenVR packages for several refresh rates / modes

Default OpenVR build can be narrowed or expanded:

```bash
cd ~/src/xr_tracking
XR_BUILD_ONLY=openvr_driver \
XR_TARGET_DEVICE=xreal_ultra \
XR_OPENVR_BUILD_FREQUENCIES="60 90" \
XR_OPENVR_BUILD_MODES="direct extended_sbs" \
./devices/xreal_ultra/linux/scripts/install_xreal_ultra_out.sh
```

Package names are generated from frequency and mode, for example:

```text
openvr_driver_60HZ
openvr_driver_90HZ
openvr_driver_60HZ_extended_sbs
openvr_driver_90HZ_extended_sbs
```

---

## 3. Important refresh-rate rule

Use one source of truth for the real glasses mode:

```bash
XR_DISPLAY_FREQUENCY_HZ=60
```

or:

```bash
XR_DISPLAY_FREQUENCY_HZ=90
```

OpenVR inherits this through `XR_OPENVR_DISPLAY_FREQUENCY_HZ` unless overridden.

Monado inherits this through:

```bash
XR_MONADO_REFRESH_HZ="${XR_MONADO_REFRESH_HZ:-${XR_DISPLAY_FREQUENCY_HZ:-90}}"
```

Do not run the glasses physically at 60 Hz while telling Monado/OpenVR that the HMD is 90 Hz. That creates wrong frame timing/prediction and can cause uneven pacing/stutter.

For 60 Hz:

```bash
XR_DISPLAY_FREQUENCY_HZ=60
```

For 90 Hz:

```bash
XR_DISPLAY_FREQUENCY_HZ=90
```

---

## 4. Run XR backend client

```bash
cd ~/src/xr_tracking/out/xreal_ultra
./run_xr_client.sh
```

---

# SteamVR / OpenVR

## 1. Register OpenVR driver — 90 Hz direct mode

Use this for direct USB4/iGPU path. Tested with HX370 iGPU.

```bash
cd ~/src/xr_tracking
XR_TARGET_DEVICE=xreal_ultra \
XR_DISPLAY_FREQUENCY_HZ=90 \
XR_OPENVR_DISPLAY_FREQUENCY_HZ=90 \
XR_OPENVR_DISPLAY_MODE=direct \
./devices/xreal_ultra/linux/scripts/openvr_driver/register_driver.sh
```

## 2. Register OpenVR driver — 60 Hz direct mode

```bash
cd ~/src/xr_tracking
XR_TARGET_DEVICE=xreal_ultra \
XR_DISPLAY_FREQUENCY_HZ=60 \
XR_OPENVR_DISPLAY_FREQUENCY_HZ=60 \
XR_OPENVR_DISPLAY_MODE=direct \
./devices/xreal_ultra/linux/scripts/openvr_driver/register_driver.sh
```


## 3. RUN direct mode via adapter dGPU

Example for RTX6000 with DisplayPort -> Type-C adapter. This mode sets `non-desktop=1` for glasses.

```bash
cd ~/src/xr_tracking/out/xreal_ultra

XR_OPENVR_DGPU_OUTPUT=DP-4 \
XR_OPENVR_LAUNCH_MODE=steam \
XR_OPENVR_CLEAR_LOGS=1 \
XR_DISPLAY_FREQUENCY_HZ=60 \
./run_openvr_dgpu_direct.sh 60
```

---

## SteamVR dev apps

```bash
cd ~/src/xr_tracking/out/xreal_ultra
```

Stereo video overlay:

```bash
./run_steamvr_video_overlay.sh
```

3D spatial points:

```bash
SPATIAL_SCENE_DRAW_POINTS=1 ./run_steamvr_spatial_scene.sh
```

Simple spatial overlay:

```bash
./run_steamvr_spatial_overlay.sh
```

---

## Restore glasses desktop

```bash
cd ~/src/xr_tracking/out/xreal_ultra

XR_STEAMVR_RESTORE_OUTPUT=DP-4 \
XR_STEAMVR_RESTORE_MAIN_OUTPUT=DP-6 \
XR_STEAMVR_RESTORE_LAYOUT=right-of \
./run_openvr_restore_desktop.sh
```

---

# Monado / OpenXR

## Current device/profile model

The Monado runtime driver is a generic runtime-stream consumer. It does not hardcode `xreal_ultra` in the C++ driver logic.

Device-specific package/start scripts are selected through:

```bash
XR_MONADO_DEVICE=xreal_ultra
XR_TARGET_DEVICE=xreal_ultra
```

XREAL Ultra display/FOV/refresh parameters live in:

```bash
devices/xreal_ultra/xreal_ultra.env
```

Important Monado display profile variables:

```bash
XR_MONADO_RENDER_WIDTH=1920
XR_MONADO_RENDER_HEIGHT=1080
XR_MONADO_WINDOW_WIDTH=3840
XR_MONADO_WINDOW_HEIGHT=1080
XR_MONADO_DISPLAY_WIDTH_M=0.120
XR_MONADO_DISPLAY_HEIGHT_M=0.034
XR_MONADO_IPD_M=0.064
XR_MONADO_LENS_VERTICAL_POSITION_M=0.017
XR_MONADO_FOV_LEFT=1.0
XR_MONADO_FOV_RIGHT=1.0
XR_MONADO_FOV_UP=1.0
XR_MONADO_FOV_DOWN=1.0
XR_MONADO_REFRESH_HZ=90
```

Normally set only `XR_DISPLAY_FREQUENCY_HZ=60` or `90`; `XR_MONADO_REFRESH_HZ` inherits from it.

---

## Build Monado driver only

```bash
cd ~/src/xr_tracking
XR_BUILD_ONLY=monado_driver \
XR_TARGET_DEVICE=xreal_ultra \
XR_MONADO_DEVICE=xreal_ultra \
./devices/xreal_ultra/linux/scripts/install_xreal_ultra_out.sh
```

---

## Monado with main monitor — fullscreen XCB fix

Example for 90 Hz XREAL mode:

```bash
cd ~/src/xr_tracking

XR_TARGET_DEVICE=xreal_ultra \
XR_MONADO_DEVICE=xreal_ultra \
XR_DISPLAY_FREQUENCY_HZ=90 \
XR_TRACKING_MONADO_COMPOSITOR_MODE=xcb_fullscreen \
XR_TRACKING_MONADO_XCB_OUTPUT=DisplayPort-1-1 \
XR_TRACKING_MONADO_XCB_RATE_HZ=90 \
./devices/xreal_ultra/linux/scripts/monado_driver/double_display_fix.sh
```

After stopping Monado driver with `Ctrl+C`, the main display should be restored.

---

## Monado with main monitor — XCB windowed mode

```bash
sudo apt install xdotool wmctrl

cd ~/src/xr_tracking/out/xreal_ultra

XR_TARGET_DEVICE=xreal_ultra \
XR_MONADO_DEVICE=xreal_ultra \
XR_DISPLAY_FREQUENCY_HZ=90 \
XR_TRACKING_MONADO_COMPOSITOR_MODE=xcb \
XR_TRACKING_MONADO_XCB_OUTPUT=DisplayPort-1-1 \
./devices/xreal_ultra/linux/scripts/monado_driver/start_monado_driver.sh
```

---

## Monado with manually disabled main monitor

Check displays:

```bash
xrandr --query | grep " connected"
```

Example output:

```text
DP-6 connected primary 7680x2160+0+1080
DisplayPort-1-1 connected 3840x1080+1920+0
```

Here:

```text
DisplayPort-1-1 = glasses
DP-6 = main display
```

Disable main display:

```bash
cd ~/src/xr_tracking

XR_TRACKING_MONADO_XCB_OUTPUT=DisplayPort-1-1 \
XR_TRACKING_MAIN_OUTPUT=DP-6 \
out/xreal_ultra/devices/xreal_ultra/linux/scripts/monado_driver/main_display_control.sh off-main
```

Run Monado driver:

```bash
cd ~/src/xr_tracking

XR_TARGET_DEVICE=xreal_ultra \
XR_MONADO_DEVICE=xreal_ultra \
XR_DISPLAY_FREQUENCY_HZ=90 \
XR_TRACKING_MONADO_COMPOSITOR_MODE=xcb_fullscreen \
out/xreal_ultra/devices/xreal_ultra/linux/scripts/monado_driver/start_monado_driver.sh
```

Run simple OpenXR app:

```bash
XR_RUNTIME_JSON=/home/vlados/src/xr_tracking/third_party/monado_driver/build/xr_tracking_relwithdebinfo/openxr_monado-dev.json \
hello_xr -G Vulkan
```

Enable main display again:

```bash
XR_TRACKING_MONADO_XCB_OUTPUT=DisplayPort-1-1 \
XR_TRACKING_MAIN_OUTPUT=DP-6 \
out/xreal_ultra/devices/xreal_ultra/linux/scripts/monado_driver/main_display_control.sh on-main
```

---

## Monado with only glasses

```bash
cd ~/src/xr_tracking

XR_TARGET_DEVICE=xreal_ultra \
XR_MONADO_DEVICE=xreal_ultra \
XR_DISPLAY_FREQUENCY_HZ=90 \
XR_TRACKING_MONADO_COMPOSITOR_MODE=xcb_fullscreen \
out/xreal_ultra/devices/xreal_ultra/linux/scripts/monado_driver/start_monado_driver.sh
```

Run simple OpenXR app:

```bash
XR_RUNTIME_JSON=/home/vlados/src/xr_tracking/third_party/monado_driver/build/xr_tracking_relwithdebinfo/openxr_monado-dev.json \
hello_xr -G Vulkan
```

##run OpenVR games through any OpenXR runtime without running SteamVR.
Check /readme/xrizer.md


---

# Windows

## Build Windows package

```powershell
cd C:\src\xr_tracking

powershell -ExecutionPolicy Bypass -File .\devices\xreal_ultra\windows\scripts\install_xreal_ultra_out.ps1 `
  -Root C:\src\xr_tracking `
  -Device xreal_ultra `
  -BuildOnly capture_service_cpp,openvr_driver
```

Build OpenVR for selected frequencies:

```powershell
cd C:\src\xr_tracking

powershell -ExecutionPolicy Bypass -File .\devices\xreal_ultra\windows\scripts\install_xreal_ultra_out.ps1 `
  -Root C:\src\xr_tracking `
  -Device xreal_ultra `
  -OpenVrFrequencies 60,90 `
  -BuildOnly openvr_driver
```

## SteamVR registration — Windows

90 Hz:

```powershell
powershell -ExecutionPolicy Bypass -File .\devices\xreal_ultra\windows\scripts\openvr_driver\register_driver.ps1 `
  -Root C:\src\xr_tracking `
  -Device xreal_ultra `
  -FrequencyHz 90
```

60 Hz:

```powershell
powershell -ExecutionPolicy Bypass -File .\devices\xreal_ultra\windows\scripts\openvr_driver\register_driver.ps1 `
  -Root C:\src\xr_tracking `
  -Device xreal_ultra `
  -FrequencyHz 60
```

## Run Windows package

```powershell
cd C:\src\xr_tracking\out\xreal_ultra_windows

$env:RUN_WSL2_RUNTIME="1"
powershell -ExecutionPolicy Bypass -File .\run_xr_client.ps1
```
