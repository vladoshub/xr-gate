# capture_service_cpp

Experimental native C++ capture service for XREAL Air 2 Ultra.

The default, known-good fallback remains `capture_service_python`. This backend is opt-in and is used to test a direct native camera session that avoids some GStreamer/Python camera-session failure modes, including sessions that keep producing black frames until the service is restarted.

Compatibility target:

```text
/tmp/capture_service_streams.json
camera0            IMAGE 480x640 GRAY8
camera1            IMAGE 480x640 GRAY8
xreal_raw_hid      BYTES
imu0               IMU_F32_LE
```

## Transports

Linux:

```text
SHM: supported
TCP: supported
```

Windows:

```text
SHM: intentionally unsupported
TCP: supported
```

TCP uses the same `capture_net_v1_json_payload` protocol as the Python `capture_service` TCP publisher, including `CAPHELLO`, `SUBSCRIBE`, and `CAPMSG1` messages.

## Camera orientation

The direct raw camera decoder outputs eye frames in the XREAL raw camera convention. For the current XREAL Air 2 Ultra profile, `camera0` uses `ccw90`, while `camera1` uses `ccw90` plus a post-rotation `xy` flip. The flip is needed because the right-eye image is portrait but upside down after the 90-degree rotation.

Runtime overrides:

```bash
XR_CAPTURE_CPP_LEFT_ROTATE=0|90|cw90|270|ccw90|180
XR_CAPTURE_CPP_RIGHT_ROTATE=0|90|cw90|270|ccw90|180
XR_CAPTURE_CPP_LEFT_FLIP=none|x|y|xy
XR_CAPTURE_CPP_RIGHT_FLIP=none|x|y|xy
```

Legacy variable names are also accepted: `CPP_CAPTURE_LEFT_ROTATE`, `CPP_CAPTURE_RIGHT_ROTATE`, `CPP_CAPTURE_LEFT_FLIP`, and `CPP_CAPTURE_RIGHT_FLIP`.

To test another right-eye convention without rebuilding:

```bash
XR_CAPTURE_CPP_RIGHT_ROTATE=ccw90 \
XR_CAPTURE_CPP_RIGHT_FLIP=none \
CAPTURE_SERVICE_IMPL=cpp \
./run_xr_client.sh
```

## Layout

```text
include/capture_service_cpp/common.hpp
include/capture_service_cpp/shm_publisher.hpp
include/capture_service_cpp/tcp_fanout.hpp
include/capture_service_cpp/stream_publishers.hpp
include/capture_service_cpp/camera_pipeline.hpp
include/capture_service_cpp/imu_pipeline.hpp
include/capture_service_cpp/platform/*        platform seams: camera open, HID access, process id, runtime defaults, POSIX/Windows SHM
include/capture_service_cpp/vendor/*          XREAL-only decoder/profile/IMU codec/stream specs
src/common.cpp
src/tcp_fanout.cpp
src/stream_publishers.cpp
src/camera_pipeline.cpp                         camera pipeline orchestration; no Linux open logic
src/imu_pipeline.cpp                            IMU pipeline orchestration; no hidapi packet parsing constants
src/main.cpp
src/platform/linux/*
src/platform/windows/*
src/vendor/*
```

The stream contracts and defaults remain unchanged: Linux still defaults to SHM, Windows still defaults to TCP-only, and XREAL stream ids remain `camera0`, `camera1`, `xreal_raw_hid`, and `imu0`.

## Build on Linux

```bash
capture_service_cpp/scripts/linux/build_capture_service_cpp.sh
```

## Run on Linux with SHM

```bash
CAPTURE_SERVICE_IMPL=cpp \
PUBLISH=shm \
devices/xreal_ultra/linux/scripts/capture_service/start_capture_service.sh
```

## Run on Linux with TCP

```bash
CAPTURE_SERVICE_IMPL=cpp \
PUBLISH=tcp \
TCP_PORT=45660 \
devices/xreal_ultra/linux/scripts/capture_service/start_capture_service.sh
```

## Build on Windows

Native Windows support is experimental and TCP-only. OpenCV may or may not expose the raw XREAL UVC frame in the same layout as Linux; if it returns converted BGR/RGB frames, the raw XREAL decoder will reject frames.

```powershell
capture_service_cpp\scripts\windows\build_capture_service_cpp.ps1 `
  -OpenCvDir C:\path\to\opencv\build `
  -HidApiRoot C:\path\to\hidapi
```

## Run on Windows with TCP

```powershell
capture_service_cpp\scripts\windows\start_capture_service_tcp.ps1 `
  -TcpPort 45660 `
  -CameraIndex 0 `
  -CameraApi msmf
```

## Third-party

The camera raw-frame reorder logic is based on MIT-licensed `nrealAirLinuxDriver` work. The upstream code is kept under `third_party/nrealAirLinuxDriver`, and the license notice is preserved in `THIRD_PARTY_NOTICES.md`.

This driver is unofficial. MCU tools are separate, opt-in only, and must not be used by the normal runtime path.

## Switch back

```bash
CAPTURE_SERVICE_IMPL=python \
devices/xreal_ultra/linux/scripts/capture_service/start_capture_service.sh
```

Default Linux runtime uses `CAPTURE_SERVICE_IMPL=cpp` and `PUBLISH=shm`. Switch back with `CAPTURE_SERVICE_IMPL=python` only after building the Python fallback explicitly.
