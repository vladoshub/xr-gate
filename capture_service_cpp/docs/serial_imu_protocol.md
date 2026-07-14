# `xr_imu_v1` serial protocol

`xr_imu_v1` carries raw HMD IMU samples from a microcontroller over UART or USB CDC. Values are already converted to SI units, but no gyro-bias subtraction or orientation fusion is performed. Basalt therefore receives the same logical payload as it receives from the XREAL HID source.

All multi-byte fields are little-endian. Every packet is 48 bytes:

| Offset | Type | Field |
|---:|---|---|
| 0 | `char[4]` | Magic: `XIMU` |
| 4 | `uint8` | Version: `1` |
| 5 | `uint8` | Flags; bit 0 means device timestamp is valid |
| 6 | `uint16` | Packet size: `48` |
| 8 | `uint32` | Monotonic source sequence |
| 12 | `uint64` | Sensor acquisition timestamp in microseconds |
| 20 | `float32[3]` | Gyroscope XYZ in rad/s |
| 32 | `float32[3]` | Accelerometer XYZ in m/s² |
| 44 | `uint32` | IEEE CRC32 over bytes `[0, 44)` |

The timestamp must be captured when the IMU sample is read or placed into the MCU FIFO, not when the UART packet is transmitted. `capture_service_cpp` maps this device clock into the host monotonic clock domain and publishes the unchanged existing `IMU_F32_LE` payload.

For firmware bring-up, `protocol: csv_f32` is also supported:

```text
timestamp_us,gx,gy,gz,ax,ay,az
```

or with an explicit sequence:

```text
timestamp_us,sequence,gx,gy,gz,ax,ay,az
```
