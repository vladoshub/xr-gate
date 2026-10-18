# XR Gate в Docker: TCP runtime

Папка `docker/` в корне репозитория; все команды ниже выполняются из неё. Комплект для [XR Gate](https://github.com/vladoshub/xr-gate), релиз **v2.3.0**, Ubuntu 24.04, Linux x64 / **x86-64-v3 (AVX2)**. Исходники проекта не меняются. Docker скачивает готовый runtime и Mercury-модели с GitHub, проверяет SHA-256 из `release.lock.json`, устанавливает зависимости из того же релиза. Компиляции XR Gate нет.

Русский · [English](README.en.md). Откройте **START-HERE.html** для выбора языка.

**Windows: [инструкция Docker Engine в WSL](docs/WINDOWS.ru.md).**

## Что запускается

Один контейнер с обычным `xr_client`, capture, Basalt, Mercury и `runtime_adapter`. Между ними сохраняются штатные SHM-каналы. У `runtime_adapter` включён дополнительный сервер приложений: JSON Lines, протокол `xr_gate_app_v1`, TCP **45810**.

| Соединение | Адрес |
| --- | --- |
| Приложение Linux/Windows → runtime | `127.0.0.1:45810` |
| Прослушивание внутри контейнера | `0.0.0.0:45810` |
| Публикация Docker на хосте | только `127.0.0.1:45810` |
| Видео TCP, если явно включить | только `127.0.0.1:45700` |

Внутренний bind на `0.0.0.0` нужен для перенаправления Docker. Доступ с LAN не включается. Используется обычная bridge-сеть; `--network host`, `--privileged` и публикация UDP не нужны. Для localhost требуется **Docker Engine 28+**: в старых версиях Docker была особенность доступа из того же L2-сегмента.

SteamVR-драйвер не собирается, не регистрируется и не запускается. Уже находящиеся в upstream-архиве файлы остаются в образе; Windows-драйвер в этот комплект не входит.

## Быстрый запуск на Linux

Нужны локальный Docker Engine 28+, Compose plugin, Python 3 и подключённый XREAL Air 2 Ultra. Целевой хост установочного скрипта — Ubuntu 24.04 x64. Другой Linux-хост с подходящим Docker может использовать тот же образ.

Распакуй архив, открой каталог `xr-gate-docker` и выполни:

```bash
chmod +x xr-gate scripts/*.sh image/*.sh
# Если Docker Engine и зависимости USB/Bluetooth ещё не установлены:
sudo bash scripts/install-engine-ubuntu.sh

cp .env.example .env
sudo ./xr-gate build
sudo ./xr-gate check-image
sudo ./xr-gate doctor
sudo ./xr-gate run
```

Сборке образа нужен интернет: GitHub releases и Ubuntu/Docker package repositories. ZIP не содержит многомегабайтные бинарники; они загружаются при сборке. Для другого релиза нужно осознанно обновить lock-файл, image tag и проверить совместимость.

`run` запускает контейнер в текущем терминале с stdin и TTY. Видны меню и консоль `xr_client`; работают его обычные команды. Меню XREAL из релиза сохранено. Для использования текущего режима дисплея выбери соответствующий пункт, либо:

```bash
sudo ./xr-gate run -- --prestart-option current
```

Не закрывай камеры и выполни подсказки startup gate. TCP-сервер запускается на этапе старта `runtime_adapter`, после готовности предыдущих сервисов. Проверять порт до этого этапа рано.

Во втором терминале:

```bash
python3 scripts/tcp_probe.py
```

Этот тест проверяет `hello` и получение кадра протокола. Наличие кадра ещё не означает корректный трекинг: приложение должно учитывать `pose_valid`, tracking status и признаки доступности рук.

## Консоль и фон

```bash
sudo ./xr-gate up        # Фоновый запуск; меню ждёт ввода
sudo ./xr-gate console   # Подключить stdin/stdout существующего xr_client
sudo ./xr-gate logs      # Только просмотр stdout/stderr
sudo ./xr-gate stop      # Штатная остановка, затем удаление контейнера
```

Для отключения терминала без остановки контейнера нажми **Ctrl+P, затем Ctrl+Q**. `Ctrl+C` в интерактивной консоли предназначен для остановки. Обычный `docker compose up` не передаёт stdin так, как интерактивный `run`; для ввода используй `console`.

Контейнер называется `xr-gate`. Можно также подключиться через `docker attach --sig-proxy=false xr-gate`. Одновременно редактировать консоль из нескольких терминалов не следует.

Логи сервисов сохраняются в `logs/`, где релизный конфиг использует стандартный `/tmp/xr_backend_client_logs`. Пользовательский `log_dir` в другом месте потребует своего mount. Настройки контроллеров внутри контейнера хранятся в Docker volume `xr-gate_controller-state`; `stop` этот volume не удаляет.

## Конфиги с хоста

Без пользовательских файлов запускается **неизменённый `xreal_ultra.json` из релизного архива**. Capture, calibration, tracking и остальные значения также остаются релизными. Добавляются только переменные включения TCP; при отдельном включении видео — его TCP-output.

| Файл на хосте | Действие |
| --- | --- |
| `config/xr_client.json` | Используется вместо выбранного релизного профиля |
| `config/override_controller.json` | Копируется в `CONFIG_PATH` сервиса контейнера при старте: Gear VR — `gearvr.json`, evdev — `default.json` |
| `.env` | Параметры Docker-комплекта: порты, USB, пути, профиль |

Каталог `config` монтируется read-only. Экспортировать исходные конфиги для редактирования:

```bash
sudo ./xr-gate export-configs
# Команда создаёт exported-configs/; существующий каталог не перезаписывает.
cp exported-configs/xr_client/xreal_ultra.json config/xr_client.json
```

Оба override необязательны и независимы. После изменений перезапусти контейнер. Можно указать другой каталог через `XR_CONFIG_DIR` в `.env`, включая абсолютный путь; имена файлов внутри остаются прежними. Простые литеральные `KEY=value`, без shell-подстановок; пути с пробелами заключай в кавычки.

Для контроллера по-прежнему требуется подходящая конфигурация сервиса в `xr_client.json`, включая `PROVIDERS`. Само наличие `override_controller.json` не меняет провайдер и не запускает сервис. Примеры BLE, evdev и обучение описаны в [docs/CONTROLLERS.ru.md](docs/CONTROLLERS.ru.md).

Если override-файл контроллера есть, его содержимое заново копируется при каждом старте. Если его убрать, используется последнее состояние из volume. Для полного сброса контроллера удали нужный `default.json` или `gearvr.json` внутри работающего контейнера через `shell`, предварительно сохранив нужную калибровку.

## USB XREAL

По умолчанию выбирается только VID:PID **3318:0426**. Скрипт ищет связанные `/dev/video*`, `/dev/hidraw*` и конкретный узел `/dev/bus/usb/BBB/DDD`. Весь `/dev` или все USB-устройства не передаются.

Первый UVC capture node устройства (sysfs index 0) отображается в `/dev/video0` контейнера: штатный capture-конфиг продолжает работать, даже если на хосте очки оказались `/dev/video8`. Можно выбрать конкретную камеру через `XR_CAMERA_DEVICE=/dev/video8`. Она должна относиться к указанному VID:PID. Результат отображения виден в `doctor`.

USB-параметры берутся из `.env.example`, затем переопределяются `.env` и окружением. Для другого оборудования укажи его VID:PID в `XR_USB_IDS` через запятую и выбери соответствующий runtime-профиль/конфиг. Для выбранных USB-устройств также передаются serial-узлы `ttyACM*`/`ttyUSB*`. Отдельную IMU можно добавить через `XR_SERIAL_DEVICES=/dev/serial/by-id/<id>` либо `/dev/ttyACM0,/dev/ttyUSB0`, даже если её VID:PID отличается от камеры.

`XR_REQUIRE_CAMERA=1` требует локальную камеру. HID по умолчанию необязателен; для профиля с HID задай `XR_REQUIRE_HIDRAW=1`. Для удалённого/TCP-профиля очисти `XR_USB_IDS`, задай `XR_REQUIRE_CAMERA=0` и оставь остальные selectors локальных устройств пустыми. `XR_CAMERA_DEVICE=/dev/videoN` при пустом `XR_USB_IDS` выбирает только эту камеру без автоматического проброса других камер/HID/USB. Все передаваемые узлы проверяются на существование и тип character device. Пути камеры и serial-IMU в runtime-конфиге должны соответствовать mappings контейнера.

После переподключения USB, сна Windows или повторного `usbipd attach` **останови и заново запусти контейнер**. Docker привязывает устройства при создании; скрипт заново определит актуальные номера. `docker restart` не обновляет эти mappings.

## Bluetooth

USB Bluetooth подключается к Linux-хосту; на Windows он сначала передаётся в Ubuntu WSL через `usbipd`. BlueZ работает на этом Linux-хосте, а контейнер получает его system D-Bus socket при `XR_BLUETOOTH=1`. Так Gear VR BLE использует физический адаптер через свой штатный transport.

Это рабочая архитектура комплекта: отдельный `bluetoothd` внутри контейнера не запускается. Ядро и драйвер `btusb` в любом случае принадлежат Linux/WSL. Pairing и trust выполняются в BlueZ хоста, сохраняются там и доступны `override_controller` через D-Bus. Подробности — в инструкции контроллеров.

## Видео

По умолчанию наружу опубликован только TCP приложений. Стереовидео не входит в JSON-протокол `runtime_adapter`. Capture-изображения внутри контейнера используются для VIO/hand tracking независимо от внешнего просмотра.

Если нужен отдельный видеоклиент, выставь `XR_VIDEO_TCP=1` в `.env`, затем заново запусти контейнер. Появится `127.0.0.1:45700`; у `xr_video` будет `VIDEO_OUTPUT=both`. Сам backend сохраняет штатное управление: в дефолтном профиле включается/выключается через консольную команду **6**. Для подключения нужен клиент бинарного XR video TCP протокола, например `tools/xr_video_tcp_client.py` из соответствующего релиза/репозитория. Это не H.264/RTSP и не поток отрисовки SteamVR.

## Диагностика

```bash
sudo ./xr-gate doctor
sudo ./xr-gate config       # Рендер базового Compose без hardware mappings
sudo ./xr-gate shell        # Bash в уже работающем контейнере
sudo ./xr-gate logs
lsusb
ls -l /dev/video* /dev/hidraw*
sudo modprobe uvcvideo
sudo modprobe hid_generic
sudo modprobe btusb
```

`doctor` проверяет локальный Engine, версию, устройства и, если включён Bluetooth, BlueZ/D-Bus. Нужен Engine той же Linux-системы, где видны USB-узлы. Remote Docker, rootless Docker и Docker Desktop в этом сценарии скрипт отклоняет.

Если отсутствуют UVC/HID/HCI, сначала исправь драйвер/USB-проброс на хосте. Если capture работает, но gate не проходит, проверь освещение, стабильность очков и логи. Образ использует калибровку конкретного устройства из релиза; собственную калибровку подключай через свой xr_client/device/tracking профиль и дополнительные mounts при необходимости.

Фактический объём проверки этого комплекта и ограничения окружения — в [docs/VALIDATION.md](docs/VALIDATION.md). Источники и зафиксированные версии — в [docs/SOURCES.md](docs/SOURCES.md).

## SteamVR в Windows

Windows-драйвер собирается отдельным job CI после применения сопровождающего патча. Скачайте artifact `xr-gate-steamvr-windows-x64`, распакуйте 60Hz-direct и выполните `Install-Driver.bat` в его папке `xr_tracking`. При работающем контейнере проверьте `Test-TCP.bat`, затем запустите SteamVR. [Полная инструкция, SBS, Direct Mode и удаление](docs/STEAMVR.ru.md). Direct Mode требует проверки с вашим GPU/очками; передача USB в контейнер не передаёт DisplayPort-видео.
