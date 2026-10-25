# Device-specific driver packages

Device identifiers such as `xreal_ultra` come from configuration, independently of the display label.

| Platform | Package path |
| --- | --- |
| Linux SteamVR | `bin/drivers/<device>/openvr_driver_<Hz>HZ[_<mode>]/xr_tracking/` |
| Linux Monado | `bin/drivers/<device>/monado_driver/` |
| Windows SteamVR | `drivers/<device>/<Hz>Hz-<mode>/xr_tracking/` |

Custom OpenVR profile/tag suffixes remain in variant names. Windows standalone builds use `bin/drivers/`; release archives use `drivers/`.

SteamVR devices are discovered from `drivers/openvr_driver/devices/<device>/settings/default.vrsettings`; shared defaults provide the explicit `generic` profile. An optional `drivers/openvr_driver/devices/<device>/display.yaml` supplies display/optics values. Monado devices are discovered from `drivers/monado_driver/profiles/<device>.env` with display/optics settings. Shared input/transport files are excluded. An optional sibling `<device>.yaml` supplies its display YAML.

Monado device .env defaults override the shared generic YAML; explicit/device YAML and caller environment values take priority.

Linux, ACT and Windows release CI discover these profiles automatically. Local driver-only builds:

```bash
bash drivers/build_profiles.sh steamvr
bash drivers/build_profiles.sh monado
```

`XR_DRIVER_DEVICES="device_a device_b"` restricts both matrices; `XR_OPENVR_DEVICES` and `XR_MONADO_DEVICES` restrict them separately. Existing single-device `XR_OPENVR_DEVICE` / `XR_MONADO_DEVICE` selections are supported. Unknown profiles fail before building.

In Windows, `XR_TARGET_DEVICE` in the active Hardware preset selects the device folder before refresh rate/display mode. The 60 Hz fallback stays within that device. Changing the preset checks the installed driver; its cache is also separated by device.

Linux common launch/register helpers use the selected device. Monado library paths are relative and remain valid after moving the release directory.

## По-русски

Пакеты разложены по идентификаторам устройств из конфигурации. В Windows выбор задаёт `XR_TARGET_DEVICE` активного Hardware preset; частота и режим выбираются внутри этой папки. Кэш также разделён по устройствам.

Linux, ACT и Windows CI автоматически собирают настроенные профили. Список можно ограничить через `XR_DRIVER_DEVICES` либо отдельные `XR_OPENVR_DEVICES` / `XR_MONADO_DEVICES`. Для нового профиля нужны его настройки; пакет другого устройства автоматически не подставляется.
