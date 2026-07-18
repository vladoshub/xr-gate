# `xr_controller_v1` serial protocol

`xr_controller_v1` is the current fixed-size serial transport for controller
IMU and input state. Values are encoded explicitly in little-endian order; no
packed C/C++ structure is transmitted.

Every packet is exactly 64 bytes:

| Offset | Type | Field |
|---:|---|---|
| 0 | `char[4]` | Magic: `XCTL` |
| 4 | `uint8` | Version: `1` |
| 5 | `uint8` | Flags: bit 0 timestamp valid, bit 1 controls valid, bit 2 battery valid |
| 6 | `uint16` | Packet size: `64` |
| 8 | `uint32` | Monotonic source sequence |
| 12 | `uint64` | Sensor acquisition timestamp in microseconds |
| 20 | `float32[3]` | Gyroscope XYZ in rad/s |
| 32 | `float32[3]` | Accelerometer XYZ in m/s² |
| 44 | `uint32` | Digital button-state bitmap |
| 48 | `int16[4]` | Thumbstick X/Y, trigger and grip/aux axes |
| 56 | `uint16` | Battery voltage in millivolts |
| 58 | `uint16` | Controller status bitmap; reserved in v1 |
| 60 | `uint32` | IEEE CRC32 over bytes `[0, 60)` |

The current `capture_service_cpp` serial IMU source consumes gyro,
accelerometer, sequence and acquisition timestamp. It preserves the complete
64-byte packet when raw publishing is enabled. Button, axis, battery and status
fields are already decoded by the shared protocol module, so firmware can begin
populating those fields later without changing packet framing or the IMU parser.

The timestamp must represent acquisition time, not UART transmission time. XYZ
remains the source sensor-frame order; optional `imu.transform` rotates gyro and
accelerometer immediately before normalized `IMU_F32_LE` publication.

The implementation is in:

```text
include/capture_service_cpp/protocols/xr_controller_v1.hpp
src/protocols/xr_controller_v1.cpp
```

For compatibility, `protocol: xr_imu_v1` remains accepted for the legacy
48-byte `XIMU` stream. New nRF54L15 configurations use:

```yaml
serial:
  baud_rate: 230400
  protocol: xr_controller_v1
  timestamp_mode: device
```

For firmware bring-up, `protocol: csv_f32` is also supported:

```text
timestamp_us,gx,gy,gz,ax,ay,az
```

or with an explicit sequence:

```text
timestamp_us,sequence,gx,gy,gz,ax,ay,az
```

Only complete packets that pass magic, version, size, CRC and finite-value
validation count as valid IMU activity. Partial packets and arbitrary transport
bytes do not reset `imu.stall_exit_ms`.
