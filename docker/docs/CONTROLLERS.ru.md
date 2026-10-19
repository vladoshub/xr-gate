# Контроллеры в Docker

[English](CONTROLLERS.md) · Русский · [К установке](../README.ru.md)

## 1. Подключите контроллеры

Остановите XR Gate. Для Bluetooth-контроллеров выполните на компьютере:

```bash
sudo apt-get install -y bluez
sudo systemctl enable --now bluetooth
bluetoothctl
```

Для Gear VR включайте контроллеры по одному и введите:

```text
power on
agent NoInputNoOutput
default-agent
scan le
```

Для Bluetooth-джойстика VR-PARK вместо этого используйте `agent on` и `scan on`.

Найдите адрес контроллера в результатах поиска. Подставьте его вместо `AA:BB:CC:DD:EE:FF`:

```text
pair AA:BB:CC:DD:EE:FF
trust AA:BB:CC:DD:EE:FF
```

Для VR-PARK также введите `connect AA:BB:CC:DD:EE:FF`. При необходимости повторите для второго контроллера, затем введите `scan off` и `exit`.

USB-джойстик подключите к компьютеру и пропустите Bluetooth-сопряжение.

## 2. Подготовьте настройки

После сборки образа выполните из папки `docker`:

```bash
sudo ./xr-gate export-configs
cp --no-clobber exported-configs/xr_client/xreal_ultra.json config/xr_client.json
```

Если `exported-configs` уже существует, используйте его или переименуйте перед повторным экспортом.

Для **Gear VR** выполните:

```bash
cp --no-clobber exported-configs/override_controller/empty_gearvr.json config/override_controller.json
```

Укажите `XR_BLUETOOTH=1` в `.env`.

Для **VR-PARK** выполните:

```bash
cp --no-clobber exported-configs/override_controller/empty_vrpark.json config/override_controller.json
cat /proc/bus/input/devices
```

Найдите джойстик по имени и его `eventN` в строке `Handlers`. Укажите в `.env`: `XR_INPUT_DEVICES=/dev/input/eventN`. Для двух устройств разделите пути запятой.

В `config/xr_client.json` найдите существующий сервис `override_controller`. Сохраните остальные настройки и измените:

| Настройка | Gear VR | VR-PARK |
| --- | --- | --- |
| `enabled` | `true` | `true` |
| `start_on_launch` | `false` | `false` |
| `env.CONFIG_PATH` | `~/.config/xr_tracking/override_controller/gearvr.json` | `~/.config/xr_tracking/override_controller/default.json` |
| `env.PROVIDERS` | `evdev,gearvr_ble` | `evdev` |
| `env.NON_INTERACTIVE` | `1` | `1` |

## 3. Зарегистрируйте контроллеры

Запустите XR Gate и выполните подсказки в консоли:

```bash
sudo ./xr-gate run
```

Оставьте консоль открытой. Во время регистрации ввод с контроллеров должен быть выключен; команда **5** в консоли переключает его.

В другом терминале, открытом в папке `docker`, выполните команду для своих контроллеров.

**Gear VR:**

```bash
sudo docker exec -it \
  -e TRAIN=0 -e NON_INTERACTIVE=0 -e PROVIDERS=gearvr_ble \
  -e GEARVR_INITIAL_SCAN_MS=10000 \
  -e CONFIG_PATH=/root/.config/xr_tracking/override_controller/gearvr.json \
  xr-gate \
  /opt/xr-gate/devices/common/linux/scripts/override_controller/start_override_controller.sh --connect-devices
```

**VR-PARK:**

```bash
sudo docker exec -it xr-gate /opt/xr-gate/bin/override_controller/override_controller \
  --config /root/.config/xr_tracking/override_controller/default.json --connect-devices
```

По подсказкам назначьте левое и правое устройства.

## 4. Сохраните и перезапустите

До остановки контейнера сохраните результат. Для **Gear VR**:

```bash
sudo docker cp xr-gate:/root/.config/xr_tracking/override_controller/gearvr.json config/override_controller.json
```

Для **VR-PARK**:

```bash
sudo docker cp xr-gate:/root/.config/xr_tracking/override_controller/default.json config/override_controller.json
```

После успешного копирования остановите XR Gate:

```bash
sudo ./xr-gate stop
```

В том же сервисе в `config/xr_client.json` установите `start_on_launch` в `true`. Запустите снова:

```bash
sudo ./xr-gate run
```

При следующих запусках включайте контроллеры до запуска XR Gate. Повторяйте регистрацию только при замене или переназначении контроллеров. Если у джойстика изменился `eventN`, обновите `.env` и перезапустите XR Gate.
