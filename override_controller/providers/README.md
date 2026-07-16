# Native controller providers

Provider transports are compiled into `override_controller`; no helper process
or newline-delimited JSON IPC is used.


## Common provider boundary

The core accepts only an ordered provider list and generic string options:

```text
--provider-option provider.key=value
```

Each provider parses and validates its own options, performs its own legacy
configuration migrations, names its own input codes, and publishes only the
common `InputEvent`, `InputBindingSpec`, `DeviceInfo`, and
`ControllerImuStateV1` contracts. `CompositeInputProvider` routes operations by
`DeviceInfo::provider_slot`; it does not interpret provider-specific codes.

Adding another provider therefore requires a factory registration and build
source entry, but does not require new provider fields or `if/else` branches in
`main.cpp`, `config_io.cpp`, or the launcher.

The shared C++ IMU pipeline lives in:

- `include/xr_override_controller/imu/controller_imu_processor.hpp`
- `src/imu/controller_imu_processor.cpp`

Transport providers convert native sensor packets into
`imu::RawControllerImuSample` using rad/s, m/s², and µT. The same processor is
used by the native Gear VR BLE provider and is intended for future MPU-6050
serial/BLE providers.

The Gear VR implementation is split into OS-independent and transport layers:

- `src/providers/gearvr/gearvr_input_provider.cpp` owns device state, button and
  touchpad events, AHRS, IMU V3 output, training integration, and stable identity.
- `src/providers/gearvr/gearvr_protocol.cpp` owns the packet decoder and command
  sequence.
- `src/providers/gearvr/gearvr_touchpad.cpp` owns raw/stick/D-pad conversion.
- `src/providers/gearvr/gearvr_ble_transport.hpp` is the OS transport contract.
- `src/providers/gearvr/transport/linux_gearvr_ble_transport.cpp` implements
  BlueZ over the system D-Bus.
- `src/providers/gearvr/transport/windows_gearvr_ble_transport.cpp` is currently
  an explicit placeholder for a future C++/WinRT backend.

A Windows transport only needs to expose paired-device snapshots and raw BLE
notifications through `gearvr::BleTransport`; the rest of the provider remains
unchanged.

Protocol attribution and the upstream MIT notice are retained in
`THIRD_PARTY_NOTICES.md`.
