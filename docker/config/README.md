Этот каталог можно оставить пустым.

JSON-файлы сохраняй как UTF-8 без BOM.

- `xr_client.json`: пользовательский конфиг xr_client вместо профиля из релиза.
- `override_controller.json`: копируется по CONFIG_PATH сервиса при старте: Gear VR — gearvr.json, обычный evdev — default.json. Предыдущий отличающийся файл сохраняется.

Сначала выполни `sudo ./xr-gate export-configs`, затем скопируй и отредактируй нужные файлы из `exported-configs/`. Активные конфиги комплект намеренно не создаёт. Они читаются с хоста; сам каталог монтируется read-only.

Для Gear VR BLE также нужен `XR_BLUETOOTH=1` и `PROVIDERS=evdev,gearvr_ble` в env соответствующего сервиса xr_client. См. `../docs/CONTROLLERS.ru.md`.

English: optional `xr_client.json` selects a custom client profile. Optional
`override_controller.json` is copied to its override service's `CONFIG_PATH`,
including `gearvr.json` for Gear VR. Export templates first, keep JSON UTF-8
without BOM, and configure providers/auto-start explicitly. The host config
directory is mounted read-only. See [controllers](../docs/CONTROLLERS.en.md).
