# Application network access

XR Gate can stream tracking data to applications over TCP or UDP. A separate
binary TCP stream carries stereo camera video. The application tracking server
is **off by default**; the usual Linux video launcher also defaults to SHM output.

| Data | Transport | Default endpoint when enabled | Format |
| --- | --- | --- | --- |
| HMD, hands, controller poses and input | Application TCP server | `127.0.0.1:45810` | UTF-8 JSON Lines, `xr_gate_app_v1` |
| HMD pose | Existing runtime UDP output | destination `127.0.0.1:45800` | `XRRO` header + `RuntimeHmdPoseF64V1` |
| 21-joint hands | Existing runtime UDP output | destination `127.0.0.1:45801` | `XRRO` header + `HandTrackingFrameF32V2` |
| Controller poses and input | Existing runtime UDP output | destination `127.0.0.1:45802` | `XRRO` header + `RuntimeControllerStateFrameV1` |
| Stereo camera video | Existing video TCP server | port `45700` | `XR_STEREO_VIDEO_V1`, binary |

These endpoints provide read-only data access. They do not stream an application's
rendered display, provide an OpenXR compositor, or accept controller commands.
Body trackers and spatial meshes are not included in `xr_gate_app_v1`.

## Enable application TCP

Export before launching the runtime stack normally:

```bash
export RUNTIME_APP_TCP=1
```

The runtime adapter must be restarted to apply changes. The server supports
multiple clients and works alongside SHM, SteamVR, and Monado output. Native
applications can also use it without enabling any SHM output.

After applying the source changes, rebuild the adapter from the repository root:

```bash
ROOT_PROJECT="$PWD" runtime_adapters/xr_runtime_adapter/scripts/linux/install_xr_runtime_adapter.sh
```

This installs both the executable and the updated launcher preferred by device
wrappers. A full Mercury/Basalt rebuild is not required.

Optional Linux launcher settings:

```bash
export RUNTIME_APP_TCP_HOST=127.0.0.1
export RUNTIME_APP_TCP_PORT=45810
export RUNTIME_APP_TCP_RATE=60          # maximum output rate, 1..240 Hz
export RUNTIME_APP_TCP_MAX_CLIENTS=8    # 1..32
```

For access from another machine, explicitly set the bind address to the XR Gate
machine's LAN address or `0.0.0.0`; point clients at that machine's IP address.
This API has no authentication or encryption. Use it on a trusted network or
through an SSH tunnel.

Equivalent executable options, including native Windows builds:

```text
--app-tcp --app-tcp-host 127.0.0.1 --app-tcp-port 45810 --app-tcp-rate 60 --app-tcp-max-clients 8
```

Disable again with `RUNTIME_APP_TCP=0` and restart. Without `--app-tcp`, the
adapter creates no application listener or network worker. When enabled but no
clients are connected, it skips JSON serialization and snapshot copying.

From a source checkout, try the standard-library Python example:

```bash
python3 tools/xr_app_client.py --host 127.0.0.1 --frames 100
python3 tools/xr_app_client.py --host 127.0.0.1 --json
```

Packaged builds include `./run_app_client.sh` with the same options.

## TCP protocol

Connect with an ordinary TCP socket; no subscription request is needed. The
server sends one JSON object followed by `\n` per message. Read complete lines:
one `recv()` call may contain a partial message or several messages. This is a
plain TCP protocol, not WebSocket or HTTP.

The first message is a `hello` object. Check `protocol == "xr_gate_app_v1"` before
consuming frames. It declares the runtime frame name, units, quaternion order,
maximum rate, and hand joint order. All TCP quaternions use **`[x,y,z,w]`**,
including the HMD quaternion, whose underlying binary contract uses `wxyz`.

Each subsequent `type == "frame"` object contains:

| Field | Meaning |
| --- | --- |
| `sequence` | Application snapshot sequence; gaps are expected when frames are skipped |
| `read_timestamp_ns` | Runtime tick's server monotonic timestamp, as a decimal string |
| `hmd` | Runtime pose, velocities, source/target timestamps, reset counter, flags, confidence, and `pose_valid`; `null` before any HMD source is available |
| `hands` | Runtime 21-joint hand frame, source timestamp/reset counter, hand count, status/flags/confidence, and `left`/`right` hand data |
| `controllers` | Composed runtime controller frame and `left`/`right` states: poses, validity, source mask, velocities, buttons, trigger, grip, thumbstick, and button counters |

Positions and linear velocities use metres and metres/second; angular velocities
use radians/second. Data already has the configured runtime transforms, origin,
filters, and prediction applied. Do not apply the HMD prediction a second time.
Axis directions follow the selected transform profile; the runtime frame name
alone does not override that profile.

Each hand contains `controller`, `palm`, and `wrist` poses, gesture strengths and
active booleans, plus `joints`. A pose is `{ "position": [x,y,z],
"orientation_xyzw": [x,y,z,w] }`. Joint entries additionally contain `joint_id`,
`flags`, `confidence`, and `radius_m`. The 21-joint order is wrist; four thumb;
four index; four middle; four ring; four little. Use `pose_valid`, hand status,
and flags to interpret loss; a retained numeric position alone is not evidence
of valid tracking. The server sends the same explicit no-hands frames as the
runtime output when the backend stops or reports no hands in tick mode with
`--consume-stale-as-lost` (enabled by the Linux launcher).

All `*_ns` values, button masks (`buttons`, `touches`, `changed_buttons`), and
`source_device_hash` are decimal strings, preserving 64-bit precision in
JavaScript. Use `int(value)` in Python, `BigInt(value)` in JavaScript, or
`Long.parseUnsignedLong(value)` for unsigned masks in Java. Server monotonic
timestamps are not wall-clock time and cannot be compared directly with another
machine's monotonic clock. Non-finite source floating-point values serialize as
JSON `null`; consumers should check numeric values and validity flags.

Button bits use the [controller input contract](../../../shared/include/xr_runtime/contracts/controller_input_contract.hpp):

| Bit index | Button |
| --- | --- |
| 0 | Trigger |
| 1 | Grip |
| 2 | Menu |
| 3, 4 | A, B |
| 5 | Thumbstick press |
| 6, 7, 8, 9, 10 | D-pad up, down, left, right, centre |
| 11, 12, 13 | X, Y, System |

`press_counters` and `release_counters` have 32 entries indexed by button bit.
Compare counters with the last received frame (unsigned 32-bit subtraction) to
detect short presses between snapshots. Initialize that baseline after each
connection/source change; do not interpret the initial counters as new presses.
`changed_buttons` describes a single runtime tick and can be skipped with a
snapshot, so use counters for edge detection.

Serialization and all socket operations run on one separate worker. The runtime
hands off only the latest fixed-size snapshot with a non-waiting mutex attempt.
Each client holds at most one in-progress message and one replacement snapshot;
pending snapshots are replaced by newer ones. A socket write stalled for 250 ms
disconnects that client. Clients exceeding the configured limit are closed.
Client input is unsupported and closes the connection. A slow/disconnected
client cannot block runtime publication or the other clients.

## Existing UDP output

UDP uses a configured destination rather than client connections/subscriptions.
Start a UDP receiver on the target host, then add these flags to the runtime
adapter or its launcher (replace the example address with the receiver's IP):

```bash
runtime_adapters/xr_runtime_adapter/scripts/linux/start_xr_runtime_adapter_shm.sh \
  --publish-runtime-pose-udp --runtime-pose-udp-host 192.168.1.50 --runtime-pose-udp-port 45800 \
  --publish-runtime-hand-udp --runtime-hand-udp-host 192.168.1.50 --runtime-hand-udp-port 45801 \
  --publish-runtime-controller-state-udp --runtime-controller-state-udp-host 192.168.1.50 \
  --runtime-controller-state-udp-port 45802
```

The [UDP contract](../../../shared/include/xr_runtime/net/runtime_output_udp.hpp)
defines a 48-byte packed header: magic `XRRO`, version/header size, packet type
(1 HMD, 2 hands, 3 controllers), flags, payload size, transport sequence, stream
sequence, send timestamp, and source timestamp. Header and payloads use the
existing little-endian x86/ARM contract. The complete UDP hand packet is 2248
bytes and may be fragmented on networks with a 1500-byte MTU. UDP applications
must tolerate loss/reordering and mark old data stale; TCP is simpler for full
hand skeletons. The new application server does not change this UDP wire format.

## Stereo video and bandwidth

Video already has its own [binary TCP server](../../../shared/include/xr_video/net/stereo_video_tcp.hpp).
To preserve local SHM consumers while also allowing applications to connect,
export before starting the video backend through the normal launcher:

```bash
export VIDEO_OUTPUT=both
export VIDEO_TCP_BIND=127.0.0.1   # use a LAN address or 0.0.0.0 for remote clients
export VIDEO_TCP_PORT=45700
```

Keep `VIDEO_OUTPUT=shm` for local-only use. Restart the video backend to apply.
Do not start a second video backend writing the same SHM stream.

The standard-library video client measures actual receive FPS and MB/s:

```bash
python3 tools/xr_video_tcp_client.py --host 127.0.0.1 --frames 300
```

Packaged builds include `./run_video_tcp_client.sh`. For application integration,
the example's `read_frame(socket)` returns metadata plus raw left/right pixel
buffers. C++ applications can use `xr_video::StereoVideoTcpClient::read_next()`.
The protocol is a 56-byte `XSVNET1\0` network header, a 96-byte stereo frame
header, then left pixels and right pixels. See the
[video contract](../../../shared/include/xr_video/contracts/stereo_video_contract.hpp)
for field definitions. Preserve the source timestamps when correlating video
with tracking; runtime HMD poses are already predicted to a later target time.

Raw image bandwidth per client, excluding small protocol and Ethernet overheads:

| Per-eye image | Stereo rate | MB/s (decimal) | Mbit/s |
| --- | --- | --- | --- |
| 480×640 GRAY8 | 30 FPS | 18.43 | 147.46 |
| 480×640 GRAY8 | 60 FPS | 36.86 | 294.91 |
| 480×640 RGB8/BGR8 | 60 FPS | 110.59 | 884.74 |

Formula: `width × height × bytes_per_pixel × 2 eyes × FPS`. GRAY8 is one byte
per pixel; RGB8/BGR8 is three. Two raw 60 FPS GRAY8 clients require about
590 Mbit/s of outgoing image data. One such client fits within a 1 Gbit/s wired
link in bandwidth terms, but actual FPS also depends on the sender, receiver,
copies, and other traffic. RGB8 at 60 FPS leaves little headroom on that link.
Wi-Fi requires a measurement of actual sustained throughput. The existing video
protocol sends uncompressed images; H.264/H.265, congestion control, or a WebRTC
video path would be separate additions.

Tracking stays on a separate socket and worker; video bytes do not enter the
tracking JSON stream. For applications on the same Linux machine, existing SHM
remains the path with the fewest copies.
