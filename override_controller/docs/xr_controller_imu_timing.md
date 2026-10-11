# xr_controller_v1 IMU timestamps and delivery

The XIAO provider retains the firmware's unsigned 64-bit microsecond counter in
`samples[0].device_timestamp_ticks` and `latest_device_timestamp_ticks` when the
wire `TimestampValid` flag is set, including a valid zero at MCU startup.
`CAP_DEVICE_TIMESTAMP` advertises this counter. It is device uptime, not Unix
time. The 64-byte XCTL/32-byte XCID protocols and packed 1432-byte V3 ABI are
unchanged; no firmware update is needed for these host-side fixes.

Each physical serial controller has its own `SerialImuClock`. A serial read
can contain several samples with a single receipt time. The clock mapper trains
once on the newest sample, then projects earlier samples with the same affine
scale. Signed clock offsets support MCU uptime greater than host uptime. Host
sample times remain strictly ordered within an epoch and never exceed receipt.
AHRS uses the original device intervals, independently of USB/scheduler jitter.

The clock fit estimates offset and drift from the lower envelope of receipt
times. It cannot measure a constant transport latency; allow the fit to settle
and calibrate camera/IMU latency for training. No fixed wire-time correction is
subtracted from a USB CDC sample: its configured baud rate need not equal the
physical acquisition-to-host transport speed.

Sequence wrap is normal. Duplicate sequence numbers or acquisition times are
not reprocessed. Sequence gaps increment a loss counter. Timestamp or sequence
rollback, reconnect, stale data, acquisition-time validity changes and large
device gaps start a new clock epoch and clear buffered old samples/AHRS state.
A read containing a reboot only publishes its final epoch. When the host clock
fit cannot preserve all intervals without future times, it starts a new epoch
instead of compressing the samples to +1 ns intervals.

With a missing `TimestampValid` flag, receipt time is a fallback and
`HOST_TIME_SYNCED`/`CAP_DEVICE_TIMESTAMP` are clear. Only the newest sample in
that read is kept, because the other acquisition times are unknown. Reject
these unsynchronised samples for IMU/camera training. Unrepresentable device
timestamps are discarded instead of overflowing the us-to-ns conversion.

The provider retains up to 256 individual XCTL IMU packets and publishes them
through the same queue-consumption path as Gear VR. Each V3 frame has one sensor
sample and its corresponding orientation. The latest snapshot remains
available separately. Disconnect, stale status and explicit event flushing
discard pending samples. FIFO overflow drops the oldest packets, increments a
diagnostic counter and emits a rate-limited warning.

`CAP_STREAM_DIAGNOSTICS` enables the existing reserved words:

| Word | Meaning |
| --- | --- |
| `DIAG_EPOCH` | Current timing/AHRS epoch |
| `DIAG_DROPPED_BATCHES` | Lost firmware samples, rejected time values and FIFO losses; each XCTL packet contains one IMU sample |
| `DIAG_TICKS_PER_SECOND` | 1,000,000 for a valid device clock; 0 for receipt-time fallback |

Loss diagnostics are included on the next consumed frame, even if it was queued
before the loss. The runtime's existing SHM/TCP history and discontinuity logic
preserves the serial burst and resets position prediction on epoch/loss changes.
History sequence comparisons handle 32-bit wire-counter wrap; earlier samples
are retained across the normal `0xffffffff` to `0` transition.
Use sequential SHM reads for collection and set `publish.slot_count` to at least
512 in the collection profile. Frame `timestamp_ns` remains publication time;
`source_timestamp_ns` is the newest active IMU acquisition estimate.

The capture service uses the same serial clock for device-time XCTL, XIMU and
CSV input. It drains complete packets from one read before assigning times,
retains their raw bytes and original receipt timestamp, and then returns them
in order. Partial frames/CRC errors/duplicates remain transport activity and
do not reset the valid-IMU stall timer. Explicit host timestamp mode continues
to publish receipt timestamps. The normalized capture stream has no epoch
field, so any reanchored prefix overlapping previously published timestamps
is discarded to keep VIO input strictly increasing. XREAL's HID/camera mapper
is unchanged.

## Verification

`serial_imu_clock_test` exercises negative offsets, first-read spacing,
sequence wrap, MCU reset, duplicate ticks, fallback, overflow and clock drift.
`serial_imu_timing_linux` uses real serial ports backed by Linux PTYs for XCTL
and XIMU batching, fragmentation, CRC, raw bytes and reset handling.
`xr_controller_imu_delivery_test` runs the real serial provider, publisher,
SHM reader and runtime synthesis to check burst delivery, position integration,
wire-counter wrap, two independent controllers, reconnect, FIFO overflow and jitter-independent
AHRS. Enable the existing test options for override_controller/capture_service
and `XR_RUNTIME_ADAPTER_BUILD_TESTS=ON` for the runtime test.

Both capture-service CMake and the Linux g++ build script include the shared
headers. Rebuild capture_service_cpp, override_controller and xr_runtime_adapter
after applying the host patches. Physical-board validation is still needed for
the actual firmware timestamp origin and transport latency.
