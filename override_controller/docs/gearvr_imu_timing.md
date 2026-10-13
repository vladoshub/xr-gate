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

## Stale delivery and AHRS continuity

A 250 ms receive timeout marks the public IMU state `STALE`, clears its validity
flags and sample count, and discards unpublished old batches. It retains the
AHRS orientation, gyro bias and device/host clock mapping. A receive pause alone
does not start a new epoch: buffered measurements still use their device-time
intervals when they arrive. Discarded queued batches increment the drop counter.

If the device clock advances across more than 250 ms of unobserved samples, the
provider holds its last orientation across that interval and resumes integration
using the spacing of the returning packet. It reports at least one missing batch
and discards unpublished pre-gap history. The exact number of lost notifications
is unknown. With the runtime yaw-continuity fix, a drop-counter change resets
position prediction while retaining optical yaw alignment. Motion while samples
were missing cannot be reconstructed.

A real BLE disconnect or non-rollover backwards device-clock jump still resets the AHRS and
starts a new epoch. The provider logs these reasons separately from host receive
timeouts and recovery. A periodic core reattach scan taking at least 100 ms also
logs how long it paused input polling, to help locate recurring transport stalls.

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
clocks for two controllers, reconnect, explicit flushing, delayed delivery,
stale recovery without gyro-bias reset and orientation continuity across missing
measurements.
The Linux runtime test uses the real publisher and POSIX SHM reader to verify
burst delivery, per-batch acceleration orientation, deduplication, reconnect
state reset and ring overrun/restart detection. Physical-controller validation
is still required for actual transport latency and firmware behavior.

## Gear VR firmware counter rollover

The two controllers in the 2026-10-13 capture roll their timestamp field at
419,430,400 microseconds (`25 * 2^24`, about 6 min 59.43 sec), despite the field
occupying four bytes. Treating this as ordinary uint32 time classified each
rollover as a device restart, reset AHRS/epoch, and cleared runtime world-yaw
alignment. Both controllers repeated the transition at the same boundary.

The stream extends this observed firmware period both between notifications
and between the two samples inside one notification. It accepts a firmware
rollover only when the resulting interval fits the host receive gap plus
100 ms of allowance for BLE jitter/batched delivery. Within a notification,
the existing 100 ms sample-span limit still applies. Ordinary full-uint32
rollover remains supported. A genuine reset away from the expected boundary
continues to start a new epoch; disconnects retain their existing reset policy.

Normal rollover preserves AHRS, gyro/accelerometer calibration, clock fitting,
queued history, sample intervals and epoch. `reason=clock_wrap` is logged once
per rollover with `AHRS=retained yaw=retained`. Published device ticks remain
monotonic in uint64, while the existing one-million-ticks-per-second ABI and
sample layout stay the same. A real missing interval through the boundary
still increments drop diagnostics and restarts position prediction without
clearing the confirmed yaw.

Regression tests replay all four captured clock transitions, startup and
intra-notification rollovers, pre-startup overlap, two complete firmware
periods, duplicate packets, real MCU restart, and missing/invalid records.
