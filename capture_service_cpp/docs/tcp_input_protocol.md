# TCP camera and IMU input

Capture Service accepts TCP camera and IMU data from a sender running on the
same host or on a separate sensor host such as an Android device.

> **Encryption: none.** The input uses plaintext TCP for camera frames, IMU
> samples and clock synchronization. The session ID identifies a capture run
> and provides no peer authentication. Bind the listeners to a trusted LAN
> address and restrict access to the intended senders.

## Connections

XR Gate listens on the PC and the sender opens outbound connections. An
enabled source opens its input listener when its own `driver` is `tcp`.
The primary camera, IMU and each external camera select their drivers
independently. Use loopback (`127.0.0.1`) when both processes run in the
same network environment, or bind to the receiver's LAN address for a
separate sender.

| Example port | Role in `HELLO` | Payload |
|---:|---|---|
| 45700 | `primary` | Left, right and every configured additional view in one frame set |
| 45701 | `external` | One mono image or a stereo pair per frame set, at its own rate |
| 45702 | `imu` | Optional gyroscope and accelerometer samples at the IMU rate |

An external source uses its own connection and timestamps. The primary and
additional views share one acquisition timestamp. Each additional mono view
is one named image. An additional stereo pair is two named images.

`service.tcp` configures the existing output listener for Capture Service
consumers. `camera.tcp` and `imu.tcp` configure the incoming sensor connections.

### Receiver configuration

This Linux example expects 480×640 grayscale frames per eye and one
additional mono IR view. `primary` gives the input dimensions and `output`
gives the published dimensions after any configured transforms. For a
stereo-only device, omit `camera.tcp.views` and
`camera.output.additional_cameras`.

```yaml
version: 1

service:
  publish: [shm]

camera:
  enabled: true
  driver: tcp
  layout: stereo
  timestamp_mode: sender_capture
  slot_count: 8
  primary:
    width: 480
    height: 640
  tcp:
    bind_host: 192.168.1.20
    port: 45700
    views:
      hand_ir:
        width: 640
        height: 480
  output:
    width: 480
    height: 640
    left_stream: camera0
    right_stream: camera1
    additional_cameras:
      hand_ir:
        source: hand_ir
        stream: hand_ir

imu:
  enabled: true
  driver: tcp
  tcp:
    bind_host: 192.168.1.20
    port: 45702
  output:
    stream: imu0
```

Replace `192.168.1.20` with the PC's listener address. `imu.enabled: true`
opens the IMU port. The sender's `has_imu` flag determines whether to expect
samples for that session.

Pass a separate YAML file through `--extra-config` to configure an external
source. For external stereo, use `layout: stereo` and the stereo
`output.left_stream` and `output.right_stream` fields.

A local main camera can use `driver: opencv` or `driver: xreal_ultra` with a
TCP external camera. A TCP main camera can also run with the existing local
external-camera config. A single external file can contain both kinds of
camera. [Local external-camera config](../configs/extra_ir_cameras.yaml)

```yaml
cameras:
  room_ir:
    driver: tcp
    layout: mono
    timestamp_mode: sender_capture
    primary:
      width: 640
      height: 480
    tcp:
      bind_host: 192.168.1.20
      port: 45701
    output:
      stream: room_ir
      width: 640
      height: 480
      format: gray8
```

## XREAL Air 2 Ultra receiver example

Use the ready-to-load receiver config for the `xreal_air2ultra_unified_480`
profile. It expects already oriented 480×640 GRAY8 planes and normalized
IMU axes from the sender. The receiver applies identity transforms and
publishes the standard `camera0`, `camera1` and `imu0` streams.
[XREAL Air 2 Ultra TCP config](../configs/xreal_ultra_tcp_input.yaml)

From a Linux release directory, start XR Gate with the installed config.

```bash
CONFIG_PATH="$PWD/bin/capture_service_cpp/configs/xreal_ultra_tcp_input.yaml" \
  ./run_xr_client.sh --config xreal_ultra
```

Set both `bind_host` values to the PC's LAN address for a remote sender.
Set `has_imu: true` for Ultra video with its HID IMU. Use the matching
Ultra stereo and camera-to-IMU calibration.

## V1 wire format

The 12-byte header precedes each message body. Binary numbers are little-endian
and floats are IEEE 754 binary32. Read exactly the stated body length from the
TCP byte stream.

| Offset | Type | Field |
|---:|---|---|
| 0 | `char[4]` | ASCII magic `XRCI` |
| 4 | `uint16` | Version `1` |
| 6 | `uint16` | Message kind |
| 8 | `uint32` | Body size in bytes |

| Kind | Message | Direction | Body |
|---:|---|---|---|
| 1 | `HELLO` | Sender to XR Gate | UTF-8 JSON describing the session and role |
| 2 | `FRAME_SET` | Sender to XR Gate | `uint64 sequence`, `uint64 timestamp_ns`, then image planes in `HELLO` order |
| 3 | `IMU_SAMPLE` | Sender to XR Gate | `uint64 sequence`, `uint64 timestamp_ns`, then six `float32` values |
| 4 | `TIME_PING` | XR Gate to sender | `uint64 t0` |
| 5 | `TIME_PONG` | Sender to XR Gate | `uint64 t0`, `uint64 t1`, `uint64 t2` |
| 6 | `READY` | XR Gate to sender | Empty body |

Send `HELLO` first on each connection. Keep reading control messages and
serialize outgoing messages on each socket. XR Gate synchronizes clocks on
the primary and external sockets before sending `READY`. The sender can
open the IMU socket after the primary `READY`. When both sources use TCP,
XR Gate sends IMU `READY` once that connection matches a primary session
with `has_imu: true`. With a local main camera, a standalone TCP IMU
performs the same clock exchange on its own socket.
Wait for `READY` on each socket before sending sensor data.

```json
{
  "role": "primary",
  "session_id": "6da27b41-bf4c-4a3f-898c-257dfc05d183",
  "has_imu": true,
  "views": [
    {"id": "left", "width": 480, "height": 640, "format": "gray8"},
    {"id": "right", "width": 480, "height": 640, "format": "gray8"},
    {"id": "hand_ir", "width": 640, "height": 480, "format": "gray8"}
  ]
}
```

Use a new UUID for each capture session. The IMU `HELLO` contains `role: imu`
and the primary `session_id`. Its samples use the same sender clock as the
primary frames. An external `HELLO` uses its own session ID and `views`.
Use `left` and `right` first for stereo and `mono` for a single external view.

Pack each image as contiguous rows without stride padding. Use `gray8`
(one byte per pixel) for stereo and additional views. A mono external
source can also use `bgr8` (three bytes per pixel in B, G, R order) as
configured. A frame set contains exactly the planes declared in `HELLO`
and has size `16 + sum(width * height * bytes_per_pixel)`. XR Gate matches
the view IDs and dimensions against its configuration, validates sizes
before allocating and publishes complete sets only. An `IMU_SAMPLE` is
40 bytes with `gx, gy, gz` in rad/s, then `ax, ay, az` in m/s².
Sequence numbers and acquisition timestamps increase per connection.
The receiver accepts HELLO bodies up to 64 KiB and derives data body sizes
from the selected source configuration.

## Clock synchronization and tracking

Stamp frames and IMU samples at acquisition. Convert hardware timestamps to
the same sender-monotonic clock used for sync replies. On Android,
`SystemClock.elapsedRealtimeNanos()` can provide that clock. [Android SystemClock](https://developer.android.com/reference/android/os/SystemClock#elapsedRealtimeNanos())

XR Gate records `t0` when it sends a ping. The sender records `t1` on receipt
and `t2` when replying, using its monotonic clock. XR Gate records `t3` when it
receives the reply. Use several exchanges to estimate the offset from the
sender clock to the PC clock and refresh the estimate during streaming.

```text
round_trip_ns = (t3 - t0) - (t2 - t1)
offset_ns = ((t0 - t1) + (t3 - t2)) / 2
```

Compute the differences as signed values. Prefer low-round-trip measurements
and use repeated exchanges to estimate drift while keeping published time
monotonic. XR Gate applies one mapping to the primary video and matching IMU
session before publishing `camera0`, `camera1` and `imu0`. An external
connection synchronizes independently.

The XR client uses capture-registry readiness for startup. Its default
Basalt mode is stereo VO when a TCP camera runs without IMU and VIO when it
runs with IMU. A local IMU can supply VIO alongside a TCP camera. Use
`--basalt-mode vo` or `--basalt-mode vio` to request a mode explicitly.
VIO waits for valid IMU samples. Video capture starts with the first
complete stereo set and continues during IMU reconnects.
[Basalt modes](../../xr_client/README.md#package-entrypoint)

For a downstream launcher on a separate host, select the Basalt mode
explicitly to match the sensor configuration.

Resend `HELLO` after reconnecting. Keep the session ID for a socket reconnect
within the same capture run. Start a new session after a clock reset or a
change in format or IMU capability. Capture Service discards incomplete
messages on disconnect and waits for another sender. The source
`stall_exit_ms` closes a connected sender that stops providing data.
The listener stays open. If the network falls behind, drop whole unsent
video sets to keep the capture queue bounded.

## Android sender integration

1. Verify the target Android device can access the sensor's USB video and
   IMU interfaces. Use USB host mode and request permission to access the
   device. [Android USB host guide](https://developer.android.com/develop/connectivity/usb/host)
2. Declare `android.permission.INTERNET` for outbound TCP. For direct LAN
   sockets on Android 17 when targeting SDK 37 or higher, also declare and
   request `ACCESS_LOCAL_NETWORK`. [Android networking](https://developer.android.com/develop/connectivity/network-ops/connecting) · [Android local network permission](https://developer.android.com/privacy-and-security/local-network-permission)
3. Decode the sensor's video into separate image planes and convert IMU
   samples to the units and axes of the XR Gate calibration. Group the
   stereo images and configured additional views before writing one
   `FRAME_SET`. Use a separate socket for the IMU samples.
4. Start the receiver and connect to its address. Verify equal timestamps for
   `camera0`, `camera1` and the additional views in the same set. If the device
   supplies IMU, check that `imu0` advances on the same clock and Basalt starts
   in VIO. Use `has_imu: false` to exercise the stereo VO path.

For XREAL Air 2 Ultra, the existing Linux device-time capture profile shows the
stereo decode, orientation and IMU source. If the sender uses its published
`unified_480` frames, send `camera0` as `left` and `camera1` as `right`,
use identity transforms at the receiver, and select the matching calibration.
At 60 FPS the two 480×640 GRAY8 planes use about 295 Mbit/s of payload.
[XREAL Ultra device-time profile](../../devices/xreal_ultra/configs/capture_service/xreal_air2ultra_linux_unified_480_device_time_shm.yaml)
