# Custom configuration

English · [Русский](#русский)

1. Run these commands from `docker/` after building the image:

   ```bash
   sudo ./xr-gate export-configs
   cp --no-clobber exported-configs/xr_client/xreal_ultra.json config/xr_client.json
   ```

2. Edit `config/xr_client.json` and save it as UTF-8 JSON. If `exported-configs` already exists, reuse it or rename it before exporting again.
3. Stop XR Gate and start it again to apply changes.

For controller settings, follow the [controller guide](../docs/CONTROLLERS.en.md). Leave this folder unchanged to use the default profile.

## Русский

1. После сборки образа выполните из папки `docker`:

   ```bash
   sudo ./xr-gate export-configs
   cp --no-clobber exported-configs/xr_client/xreal_ultra.json config/xr_client.json
   ```

2. Отредактируйте `config/xr_client.json` и сохраните JSON в UTF-8. Если `exported-configs` уже существует, используйте его или переименуйте перед повторным экспортом.
3. Остановите XR Gate и запустите снова, чтобы применить изменения.

Контроллеры настройте по [инструкции](../docs/CONTROLLERS.ru.md). Для работы со стандартным профилем ничего в этой папке менять не нужно.
