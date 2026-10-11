# Gear VR IMU timing and delivery

Gear VR notifications contain two accelerometer/gyroscope records. Their
32-bit microsecond counter is extended to 64 bits, including a rollover inside
one notification. `samples[].device_timestamp_ticks` and
`latest_device_timestamp_ticks` retain this counter. It restarts in a new epoch
after a device reset or reconnect; it is not a wall-clock timestamp.

The shared `AffineDeviceClockMapper` estimates host steady-clock nanoseconds
from device time. It observes the newest record once per notification, fits
clock drift using the lowest-delay observation in 250 ms windows, and maps the
earlier record with the same scale. Host timestamps are monotonic and never
later than notification receipt. AHRS integration uses device intervals, so
BLE jitter and clock-model updates do not change its integration step.

This is an estimate based on receive times. It cannot measure a constant BLE
transport delay or replace camera/IMU latency calibration. Allow several seconds
for the clock fit to settle before using samples for training.

## Fields in ControllerInputV3

The packed V3 layout remains unchanged (1432 bytes).

| Field | Meaning |
| --- | --- |
| `timestamp_ns` | Frame publication time, used for stream freshness |
| `source_timestamp_ns` | Newest active IMU sample across the two sides; publication time when no current IMU timestamp exists |
| `imu.sequence` | Provider notification sequence; distinct from publication sequence |
| `imu.samples[].timestamp_ns` | Estimated sample time in producer steady-clock nanoseconds |
| `imu.samples[].device_timestamp_ticks` | Extended Gear VR microsecond counter |
| `imu.orientation_timestamp_ns` | Timestamp of the newest sample used by the batch orientation |

`CAP_DEVICE_TIMESTAMP`, `CAP_MULTI_SAMPLE_PACKET` and `CAP_STREAM_DIAGNOSTICS`
are set. With the diagnostics capability, the previously reserved words carry
`DIAG_EPOCH`, `DIAG_DROPPED_BATCHES` and `DIAG_TICKS_PER_SECOND` (1,000,000 for
Gear VR). Consumers should reset integration/discard affected training segments
when an epoch or dropped-batch counter changes. Old consumers may ignore these
words.

## Queues and runtime

Each Gear VR session retains up to 256 notification batches. Publication drains
the selected devices' queues in order, with one V3 frame per batch. A physical
device is consumed once per publication even when multiple bindings reference
it or left/right sharing is enabled. `publish.rate_hz` sets the publication
schedule; queued IMU batches may produce several frames at a scheduled tick.
Buttons and axes still represent the current input state.

The runtime reads each new SHM ring slot sequentially with exact sequence and
seqlock checks. Its idle poll waits up to 1 ms rather than 20 ms. SHM and TCP
frames are retained in a 512-frame history until the next runtime tick. Position
prediction receives all new acceleration samples, ordered and deduplicated by
timestamp, with the orientation from each sample's notification batch. V3 has
one orientation per batch, not a separate quaternion for each sensor record.

First attachment starts at the latest available frame. Thereafter a ring
overrun, a publisher restart, a history overflow, or a provider epoch change is
an explicit discontinuity. The runtime resets prediction state rather than
integrating across it. Provider queue overflow drops the oldest batch and
increments diagnostics; warnings are rate-limited to once per second. Lost BLE
notifications cannot be recovered by these queues.

The bundled `empty_gearvr.json` uses 512 SHM slots. Existing user configs retain
their own `publish.slot_count`; set it to at least 512 for Gear VR collection.
Consumers that only read the latest SHM frame can still skip IMU batches and
must use sequential reads for dataset collection.

## Build and verify

Rebuild both `override_controller` and `xr_runtime_adapter` to use the complete
delivery path. Enable tests in each component:

```sh
cmake -S override_controller -B build/override_controller -DOVERRIDE_CONTROLLER_BUILD_TESTS=ON
cmake --build build/override_controller
ctest --test-dir build/override_controller --output-on-failure

cmake -S runtime_adapters/xr_runtime_adapter -B build/xr_runtime_adapter -DXR_RUNTIME_ADAPTER_BUILD_TESTS=ON
cmake --build build/xr_runtime_adapter
ctest --test-dir build/xr_runtime_adapter --output-on-failure
```

The Gear VR test exercises real packet decoding, counter rollover, reset,
duplicates/overlap, clock drift, jitter-independent AHRS and queue overflow.
A fake BLE transport also checks real provider/composite routing, independent
clocks for two controllers, reconnect and explicit flushing.
The Linux runtime test uses the real publisher and POSIX SHM reader to verify
burst delivery, per-batch acceleration orientation, deduplication, reconnect
state reset and ring overrun/restart detection. Physical-controller validation
is still required for actual transport latency and firmware behavior.
