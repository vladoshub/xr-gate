# capture_service_cpp

Native C++ capture service for XR Gate. Camera and IMU sources are selected independently at runtime and are normalized into the existing XR Gate stream contracts:

```text
camera0  IMAGE GRAY8
camera1  IMAGE GRAY8
imu0     IMU_F32_LE: gx, gy, gz [rad/s], ax, ay, az [m/s²]
```

The rest of the XR Gate pipeline does not need to know which physical camera or IMU produced the streams.

## Compatibility

When no configuration file exists, the service uses the built-in XREAL Ultra profile. This preserves the previous behavior:

```text
camera source: xreal_ultra
IMU source:    xreal_hid
camera0:       480x640 GRAY8
camera1:       480x640 GRAY8
imu0:          IMU_F32_LE
xreal_raw_hid: BYTES
```

The XREAL raw camera decoder, eye transforms, HID start command, IMU normalization, stream IDs, SHM/TCP publishers and registry format remain unchanged.

## Configuration

Default location:

```text
~/.config/xr_tracking/capture_service_cpp/config.yaml
```

An exact file can be selected from CLI:

```bash
capture_service_cpp --config-path /path/to/profile.yaml
```

A directory and file name can be supplied separately:

```bash
capture_service_cpp \
  --config-dir ~/.config/xr_tracking/capture_service_cpp \
  --config-name ultraleap_nrf54l15.yaml
```

Environment equivalents:

```text
XR_CAPTURE_CPP_CONFIG
XR_CAPTURE_CPP_CONFIG_DIR
XR_CAPTURE_CPP_CONFIG_NAME
```

Precedence is:

```text
CLI > environment > YAML > platform defaults / built-in XREAL profile
```

If an explicitly selected file does not exist, startup fails. If only the default `config.yaml` is absent, the built-in XREAL profile is used.

The parser intentionally supports the subset needed by these profiles: nested mappings, scalar values, comments, inline arrays, block scalar arrays and indented multiline scalar continuations. It does not require `yaml-cpp`.

Existing device profiles wrapped in a top-level `capture_service:` mapping remain supported. The legacy XREAL keys under `xreal_linux.camera` and `xreal_linux.imu` are translated to the runtime-independent camera/IMU configuration, so current XREAL launch scripts and profiles do not need to be rewritten immediately.

## Independent source selection

Example: normal side-by-side UVC camera plus nRF54L15 over USB CDC/UART:

```yaml
version: 1

service:
  namespace: external_stereo_nrf54l15
  registry_path: /tmp/capture_service_streams.json
  publish: [shm]

camera:
  enabled: true
  driver: opencv
  layout: side_by_side_horizontal
  stereo_order: left_right
  primary:
    device:
      linux: /dev/video2
    index: 1
    api: auto
    width: 1280
    height: 480
    fps: 90
    raw_format: false
    convert_rgb: true
  output:
    left_stream: camera0
    right_stream: camera1
    width: 640
    height: 480
  transform:
    left:
      rotate: none
      flip: none
    right:
      rotate: none
      flip: none

imu:
  enabled: true
  driver: serial
  output:
    stream: imu0
  raw:
    enabled: false
  serial:
    port:
      linux: /dev/ttyACM0
      windows: COM5
    baud_rate: 921600
    protocol: xr_imu_v1
    timestamp_mode: device
```

Example: external camera while retaining the XREAL glasses IMU:

```yaml
camera:
  driver: opencv
  layout: side_by_side_horizontal
  primary:
    device:
      linux: /dev/video2
    index: 1
  output:
    width: 640
    height: 480

imu:
  driver: xreal_hid
```

Camera and IMU source selection is never coupled.

## Camera drivers

### `xreal_ultra`

Uses the existing vendor-packed XREAL camera decoder. Default output is the verified 480x640 orientation:

```yaml
camera:
  driver: xreal_ultra
  layout: xreal_packed
```

### `opencv`

Uses normal OpenCV/V4L2/Media Foundation/DirectShow inputs. Supported layouts:

```text
side_by_side_horizontal
side_by_side_vertical
dual
```

`dual` opens `camera.primary` and `camera.secondary` as separate devices. This provides the generic cross-platform seam for future cameras. A vendor-packed Ultraleap stream can be added later as another `ICameraSource` without changing publishers or the downstream pipeline.

Final left and right dimensions must match `camera.output.width` and `camera.output.height`; the service fails rather than silently resizing calibration-sensitive camera frames.

## IMU drivers

### `xreal_hid`

Uses the existing XREAL HID path and normalization. VID, PID and interface can be overridden:

```yaml
imu:
  driver: xreal_hid
  xreal_hid:
    vendor_id: 0x3318
    product_id: 0x0426
    interface: 2
    drop_first_packets: 500
```

### `serial`

Cross-platform transport:

```text
Linux:  /dev/ttyACM*, /dev/ttyUSB*
Windows: COM*
```

Supported protocols:

```text
xr_imu_v1  binary fixed-size packet with sequence, device timestamp and CRC32
csv_f32    development text protocol
```

See [`docs/serial_imu_protocol.md`](docs/serial_imu_protocol.md).

For HMD VIO, firmware should send raw calibrated-unit gyro/accelerometer samples without Madgwick/Mahony orientation fusion or startup gyro-bias subtraction. Basalt continues to estimate IMU bias in the existing pipeline.

## CLI overrides

```text
--config PATH
--config-path PATH
--config-dir DIR
--config-name NAME
--registry PATH
--namespace NAME
--publish shm|tcp|shm,tcp
--tcp-bind HOST
--tcp-port PORT
--camera-driver xreal_ultra|opencv
--camera-layout side_by_side_horizontal|side_by_side_vertical|dual
--video-device PATH
--camera-index N
--camera-api v4l2|gstreamer|msmf|dshow|any
--secondary-video-device PATH
--secondary-camera-index N
--imu-driver xreal_hid|serial
--serial-port PORT
--serial-baud RATE
--raw-imu
--no-raw-imu
--no-camera
--no-imu
--duration SEC
```

Legacy XREAL environment overrides for camera orientation and device selection remain supported.

## Architecture

```text
config.yaml
   ├── ICameraSource
   │     ├── XrealCameraSource
   │     └── OpenCvStereoCameraSource
   └── IImuSource
         ├── XrealHidImuSource
         └── SerialImuSource
                 │
                 ▼
      normalized StereoFrame / ImuSample
                 │
                 ▼
      unchanged camera/IMU pipelines
                 │
                 ▼
      unchanged SHM/TCP publishers
                 │
                 ▼
      camera0 / camera1 / imu0
```

OS-specific code is limited to camera opening, serial ports, process/runtime defaults and SHM. Device codecs and source orchestration are platform-neutral.

## Included profiles

```text
configs/xreal_ultra.yaml
configs/opencv_sbs_nrf54l15.yaml
configs/opencv_sbs_xreal_imu.yaml
configs/opencv_dual_serial_imu.yaml
```

## Build on Linux

```bash
sudo apt install -y libopencv-dev libhidapi-dev pkg-config
capture_service_cpp/scripts/linux/build_capture_service_cpp.sh
```

## Build on Windows

```powershell
capture_service_cpp\scripts\windows\build_capture_service_cpp.ps1 `
  -OpenCvDir C:\path\to\opencv\build `
  -HidApiRoot C:\path\to\hidapi
```

Windows remains TCP-only. Camera devices use OpenCV Media Foundation, DirectShow or another configured backend; serial IMU uses the native Win32 COM API.

## Transports

```text
Linux:  SHM and TCP
Windows: TCP
```

TCP uses the existing `capture_net_v1_json_payload` protocol.


### Per-source SHM ring sizes

`service.slot_count` is the fallback. Use `camera.slot_count`, `imu.slot_count`, and
`imu.raw.slot_count` when the streams need different ring depths. The XREAL Ultra
profile keeps the historical values: 8 camera frames and 2048 normalized/raw IMU packets.

The canonical XREAL Ultra profile now uses the same `service` / `camera` / `imu` schema as
external devices. Legacy `capture_service.xreal_linux.*` keys are accepted only for
backward compatibility.

## Linux launcher

The hardware-independent runtime launcher lives with the service:

```text
capture_service_cpp/scripts/linux/start_capture_service_cpp.sh
```

It resolves the installed binary, applies generic runtime options and then
appends user CLI arguments so explicit CLI values retain highest precedence.
Device profiles should remain thin wrappers: source their device environment,
export `CONFIG_PATH` and other profile defaults, and execute the service
launcher from:

```text
$XR_BIN_ROOT/scripts/capture_service_cpp/start_capture_service_cpp.sh
```

with a source-tree fallback to the path above.

Common launcher variables:

- `CAPTURE_SERVICE_CPP_BIN`
- `CONFIG_PATH`, or `CONFIG_DIR` plus `CONFIG_NAME`
- `PUBLISH`
- `REGISTRY_PATH`
- `CAPTURE_NAMESPACE`
- `TCP_BIND_HOST`, `TCP_PORT`
- `NO_CAMERA`, `NO_IMU`, `DURATION`
- `STOP_EXISTING`
