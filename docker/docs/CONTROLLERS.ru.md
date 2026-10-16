# Физические контроллеры в Docker

Русский · [English](CONTROLLERS.en.md)

По умолчанию релизный профиль не запускает физический ввод автоматически.
Для Gear VR и VR-PARK используйте готовые шаблоны и **--connect-devices**, как
в корневом README. Полное обучение предназначено для других раскладок.
Overrides находятся на хосте; BlueZ работает на Linux/WSL-хосте.

## Gear VR: pairing на хосте

Остановите XR Gate. В Windows передайте отдельный Bluetooth-донгл в Ubuntu.
В Linux-хосте выполните:

```bash
sudo systemctl enable --now bluetooth
bluetoothctl
```

Сначала включите только первый Gear VR. Внутри bluetoothctl:

```text
power on
agent NoInputNoOutput
default-agent
scan le
```

Для найденного реального MAC выполните `pair <MAC>`, `trust <MAC>`, `info <MAC>`.
Включите второй контроллер и повторите для его MAC, затем `scan off`, `exit`.
При нескольких адаптерах сначала выберите нужный через `list` и
`select <MAC адаптера>`. В Linux-копии `.env` установите **XR_BLUETOOTH=1**.

## Подготовить готовый конфиг

После сборки образа, из папки `docker/` репозитория:

```bash
sudo ./xr-gate export-configs
cp --no-clobber exported-configs/xr_client/xreal_ultra.json config/xr_client.json
cp --no-clobber exported-configs/override_controller/empty_gearvr.json config/override_controller.json
```

Существующие export/config не перезаписываются: используйте их либо сохраните
и переименуйте перед выбором другого шаблона. В **существующем** сервисе
`override_controller` файла `config/xr_client.json` сохраните остальные поля и задайте:

- `enabled`: true.
- `start_on_launch`: **false на время регистрации**, после неё true.
- `env.CONFIG_PATH`: `~/.config/xr_tracking/override_controller/gearvr.json`.
- `env.PROVIDERS`: `evdev,gearvr_ble`.
- `env.NON_INTERACTIVE`: `1` для обычного runtime.

## Зарегистрировать устройства без обучения кнопок

Запустите `sudo ./xr-gate run`, пройдите startup gate. Во время регистрации не
включайте override-сервис командой 5. Если он уже работает, сначала остановите
этот сервис. Во втором терминале Linux:

```bash
sudo docker exec -it \
  -e TRAIN=0 -e NON_INTERACTIVE=0 -e PROVIDERS=gearvr_ble \
  -e GEARVR_INITIAL_SCAN_MS=10000 -e VERBOSE=1 \
  -e CONFIG_PATH=/root/.config/xr_tracking/override_controller/gearvr.json \
  xr-gate \
  /opt/xr-gate/devices/common/linux/scripts/override_controller/start_override_controller.sh --connect-devices
```

Выполните подсказки выбора физических устройств и сторон. Используются
привязки/параметры шаблона; полного обучения всех кнопок здесь нет.

**До следующего запуска контейнера** сохраните результат в host override:
при старте он снова копируется в state volume.

```bash
sudo docker cp xr-gate:/root/.config/xr_tracking/override_controller/gearvr.json config/override_controller.connected.json
python3 -m json.tool config/override_controller.connected.json > /dev/null
cp --no-clobber config/override_controller.json config/override_controller.before-connect.json
sudo cp config/override_controller.connected.json config/override_controller.json
```

Каждая команда должна завершиться успешно. Остановите XR Gate, задайте
`start_on_launch: true` в client override и пересоздайте контейнер. Ввод будет
стартовать автоматически, команда **5** переключает его вручную. Pairing и
регистрация обычно выполняются один раз; IMU-выравнивание XR Gate при старте сохраняется.

## VR-PARK и обычный evdev

Спарьте джойстик в BlueZ хоста: `agent on`, `scan on`, затем pair/trust/connect
своего MAC. USB-джойстик должен быть передан в Linux/WSL. Найдите нужные
`/dev/input/eventN` и явно перечислите их в **XR_INPUT_DEVICES** файла `.env`.
При смене nodes пересоздайте контейнер.

Используйте **empty_vrpark.json** вместо Gear VR-шаблона. В сервисе задайте
`CONFIG_PATH: ~/.config/xr_tracking/override_controller/default.json` и
`PROVIDERS: evdev`. До регистрации оставьте автозапуск выключенным. В работающем
контейнере без второго override-процесса:

```bash
sudo docker exec -it xr-gate /opt/xr-gate/bin/override_controller/override_controller \
  --config /root/.config/xr_tracking/override_controller/default.json --connect-devices
```

Экспортируйте обновлённый **default.json** в host override аналогично примеру
выше, затем включите автозапуск и перезапустите контейнер. Шаблон может подойти
похожим джойстикам с четырьмя кнопками и mouse-stick, но это зависит от модели.

## Другие устройства: полное обучение

Для другой раскладки evdev доступен мастер из корневого README. Сначала
остановите override-сервис и сохраните существующий конфиг. Запустите тот же
common script внутри контейнера с `TRAIN=1`, `NON_INTERACTIVE=0`,
`PROVIDERS=evdev` и явным `CONFIG_PATH` к **default.json**. После успешного
обучения экспортируйте результат в host override до перезапуска. Этот режим
заменяет привязки, в отличие от **--connect-devices**.

`controller-state` volume хранит JSON контроллеров. Pairing отдельно хранится в
BlueZ хоста. Временное переподключение не требует удалять state volume.
