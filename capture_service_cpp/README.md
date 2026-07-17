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

## Capture profile metadata

Set a stable runtime profile name independently from the stream namespace:

```yaml
service:
  namespace: xr_tracking
  profile: leap_motion_uvc_nrf54l15
  publish: [shm, tcp]
```

The value is written to the local registry and to the TCP `CAPHELLO` metadata.
It selects matching backend files such as
`configs/profiles/leap_motion_uvc_nrf54l15.env`; it does not alter stream IDs or
payload formats.

The build also installs a small metadata-only client:

```bash
capture_tcp_probe \
  --host 192.168.1.20 \
  --port 45660 \
  --timeout-ms 1500 \
  --print-profile
```

It reads only `CAPHELLO`, prints the profile, sends `CLOSE`, and exits. It can
also print `--print-namespace` or the complete `--print-json` metadata. Profile
names are restricted to letters, digits, `_`, `-`, and `.`.

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
interleaved_columns
dual
```

`dual` opens `camera.primary` and `camera.secondary` as separate devices.

`interleaved_columns` accepts a raw 8-bit UVC frame whose bytes on every row are arranged as:

```text
L0 R0 L1 R1 L2 R2 ...
```

The captured OpenCV frame may be exposed as `height x width` `CV_8UC2`, `height x (2*width)` `CV_8UC1`, or as one continuous raw buffer with the same byte count. `camera.output.width` and `camera.output.height` describe one eye. Use `convert_rgb: false`; normally leave `raw_format: false` so OpenCV preserves the native UVC sample layout without requesting an opaque encoded packet buffer.

Example:

```yaml
camera:
  driver: opencv
  layout: interleaved_columns
  stereo_order: left_right
  primary:
    device:
      linux: /dev/video2
    index: 0
    api: auto
    width: 640
    height: 480
    fps: 90
    raw_format: false
    convert_rgb: false
    buffer_size: 1
  output:
    width: 640
    height: 480
```

This decoder is independently implemented as a generic byte-layout transform. XR Gate does not include or redistribute LeapUVC source code, SDK components, calibration extraction code, firmware, or unlock utilities. A user must enable UVC access separately and comply with the device vendor's terms. The XR Gate project license and `THIRD_PARTY_NOTICES.md` are unchanged.

Final left and right dimensions must match `camera.output.width` and `camera.output.height`; the service fails rather than silently resizing calibration-sensitive camera frames.

### Camera controls

Camera controls are part of the platform-neutral camera-device configuration:

```yaml
camera:
  primary:
    controls_policy: strict
    controls:
      brightness: 10
      contrast: 0
      gain: 32
      exposure_time_absolute: 5000
```

`controls_policy` may be `strict` (default) or `best_effort`. In strict mode the camera fails to start if a requested control is unavailable, read-only, outside its native range, or rejected by the backend. In best-effort mode the service logs a warning and continues.

The schema also supports platform overrides while preserving one shared profile:

```yaml
camera:
  primary:
    controls:
      brightness: 10
      linux:
        exposure_time_absolute: 5000
      windows:
        exposure: -5
```

Generic values are loaded first; the active platform mapping overrides controls with the same name. Linux currently implements controls through the native V4L2 control API and accepts the same normalized names shown by `v4l2-ctl --list-ctrls` (for example `brightness`, `gain`, `exposure_time_absolute`, `contrast`, `gamma`, and `sharpness`). The Windows contract and source boundary are present, but the Windows control backend is intentionally not implemented yet. A strict Windows profile containing controls therefore reports an explicit startup error instead of silently ignoring calibration-sensitive settings.

Controls are applied after width, height, FPS and pixel-format negotiation and before the first frame is read. Profiles without a `controls` section follow the previous code path unchanged.

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

See [`docs/serial_imu_protocol.md`](docs/serial_imu_protocol.md). The canonical wire codec is the hardware-independent `protocols/xr_imu_v1` module; the serial driver does not duplicate packet layout or CRC logic.

Only complete validated serial samples reset `imu.stall_exit_ms`. Receiving partial packets or arbitrary bytes is tracked as transport activity but does not hide a stalled/corrupt IMU source. Device timestamps are mapped into host steady time with a bounded affine clock model that estimates both offset and oscillator-rate error.

For HMD VIO, firmware should send raw calibrated-unit gyro/accelerometer samples without Madgwick/Mahony orientation fusion or startup gyro-bias subtraction. Basalt continues to estimate IMU bias in the existing pipeline.


### IMU frame transform

`capture_service_cpp` is the only layer that may rotate IMU axes. Firmware and
source drivers publish their native XYZ order; consumers receive the normalized
`imu0` frame. Raw IMU packets are never modified.

Omitting `imu.transform` is an identity transform. This preserves existing
XREAL Ultra profiles and legacy configs exactly:

```yaml
imu:
  driver: xreal_hid
  # no transform: native XYZ is published unchanged
```

For a rigid mount aligned in 90-degree increments, use a signed axis mapping:

```yaml
imu:
  transform:
    axes: [x, -z, y]
```

The list describes output axes, so the example means:

```text
out.x =  in.x
out.y = -in.z
out.z =  in.y
```

Every source axis must appear exactly once, and the mapping must be a proper
right-handed rotation. Reflections such as `[x, y, -z]` are rejected.

For an arbitrary rigid mounting angle, use a quaternion in `x, y, z, w` order:

```yaml
imu:
  transform:
    quaternion_xyzw: [qx, qy, qz, qw]
```

The quaternion is normalized at startup and represents the rotation from the
source IMU frame to the published output frame. `axes` and `quaternion_xyzw`
are mutually exclusive; configuring both is a startup error. The same rotation
is applied to gyro and accelerometer vectors immediately before normalized
`imu0` publication.

#### Guided IMU mount calibration

Use the SHM calibration tool after mounting an external IMU:

```bash
python3 tools/calibrate_imu_axes.py
```

The tool reads the normalized `imu0` `IMU_F32_LE` stream directly from
`/tmp/capture_service_streams.json` and the associated POSIX SHM ring. The
verified XREAL Ultra axis layout is used as the reference:

```text
pitch up   -> +X
yaw right  -> -Y
roll right -> -Z
```

Before calibration, remove/comment `imu.transform` in the tested profile and
restart `capture_service_cpp`. The 30-second guided procedure prints one
ready-to-paste result:

- no transform when the source already matches XREAL Ultra;
- `axes: [...]` for a mount close to 90-degree increments;
- `quaternion_xyzw: [...]` for an arbitrary rigid mounting angle.

The script also writes a JSON report to
`/tmp/capture_imu_mount_calibration.json`. Override paths when needed:

```bash
python3 tools/calibrate_imu_axes.py \
  --registry /tmp/capture_service_streams.json \
  --stream imu0 \
  --output /tmp/my_imu_mount.json
```

A deterministic dependency-free check is available with:

```bash
python3 tools/calibrate_imu_axes.py --self-test
```

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
--camera-layout side_by_side_horizontal|side_by_side_vertical|interleaved_columns|dual
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
configs/opencv_interleaved_columns_xreal_imu.yaml
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

## Capture profile metadata

A configuration can identify the complete camera/IMU/mounting mode independently
from the SHM namespace:

```yaml
service:
  namespace: xr_tracking
  profile: leap_motion_uvc_nrf54l15
```

The service publishes this value at the top level of the stream registry:

```json
{
  "namespace": "xr_tracking",
  "profile": "leap_motion_uvc_nrf54l15",
  "streams": {}
}
```

Backend launchers load an exact `<profile>.env` from their own
`configs/profiles/` directory. `--profile`, `CAPTURE_PROFILE`, and
`CPP_CAPTURE_PROFILE` can override the YAML value. Namespace and profile remain
separate: namespace identifies the stream set, while profile selects compatible
calibration and backend settings.
