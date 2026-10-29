# Monado: spatial entities, исходная геометрия, видео и TCP

Изменения подготовлены для XR Gate main `3b1e5d76a30152e752e650e31831e20c9b3ac445`
и закреплённого Monado `7363fee94b66671efdce79655b8b143d7c9eeecd`.

По умолчанию транспорт остаётся SHM. Spatial и видео выключены. Контроллеры и
опциональный `XR_EXT_hand_tracking` сохраняют прежние настройки.

## Что получает приложение

При `XR_MONADO_SPATIAL=1` Monado объявляет:

- `XR_EXT_spatial_entity` (с зависимостью `XR_EXT_future`);
- `XR_EXT_spatial_mesh_tracking`;
- `XR_EXT_spatial_entity_archetypes`;
- экспериментальное проектное расширение `XR_MNDX_xr_gate_data`.

Mesh tracking предоставляет одну обновляемую entity для текущей видимой геометрии,
с компонентами `BOUNDED_3D` и `MESH_3D`. Вершины конечны, индексы преобразованы в
`uint32`, повреждённые/вырожденные треугольники исключены. Координаты согласуются
с запрошенными `baseSpace` и `time` через space overseer Monado, включая recenter,
смещение reference space и tracking origin. Данные snapshot неизменны до его
уничтожения; размеры буферов запрашиваются стандартным вызовом с capacity=0.

ID сохраняется в пределах непрерывного периода валидного трекинга. При временном
отсутствии mesh entity становится PAUSED; после потери и восстановления HMD
старый ID становится STOPPED, новый mesh получает новый ID. Можно запрашивать
обновления нескольких старых/текущих entities за один вызов. Контексты, snapshots,
entity handles и futures очищаются при завершении сессии/instance.

Это геометрия текущего обзора, а не накопленная модель комнаты. Плоскости,
семантика «стол/стена», watertight mesh, пространственные якоря и сохранение карты
между сессиями не объявляются. Приложение должно поддерживать mesh tracking;
поддержка только plane tracking или собственных расширений другого производителя
не обеспечивает совместимость автоматически.

`XR_MNDX_xr_gate_data` — экспериментальное расширение XR Gate, не зарегистрированное
Khronos. Оно предоставляет immutable CPU snapshots через:

- `xrCreateDataSnapshotMNDX`;
- `xrGetDataSnapshotPropertiesMNDX`;
- `xrGetSpatialMeshMNDX`;
- `xrGetStereoVideoMNDX`;
- `xrDestroyDataSnapshotMNDX`.

Геометрия отдаётся точной копией `RUNTIME_SPATIAL_PROXY_MESH_F32_V2`, включая
исходную сетку, NaN и треугольники, которые стандартный mesh-путь отфильтровал.
Видео отдаётся как `StereoVideoFrameHeaderV1` + left bytes + right bytes.
Пиксели, pixel_format, stride и source timestamps сохраняются. Источники часов
монотонные; source timestamps не являются OpenXR `XrTime`.

Приложение может загрузить видеокадры в свои текстуры и отрисовать passthrough.
Эта версия не реализует автоматический compositor passthrough через
`XR_FB_passthrough`. Для точного совмещения видео с изображением очков приложению
потребуются калибровка камер и camera-to-eye transform: текущий видеоконтракт
не содержит их. Заголовок расширения и headless-пример приложения устанавливаются
в `bin/drivers/monado_driver/include/` и `examples/`.

## Сборка

Применение патча из архива:

```bash
cd ~/src/xr-gate
git apply --check /path/to/xr-gate-monado-spatial-video-tcp.patch
git apply /path/to/xr-gate-monado-spatial-video-tcp.patch
```

Пересобрать Monado в привычный release-каталог:

```bash
cd ~/src/xr-gate
BIN_DIR="$PWD/out/xr-gate/bin/drivers/monado_driver" \
XR_BIN_ROOT="$PWD/out/xr-gate/bin" \
XR_MONADO_DEVICE=xreal_ultra \
UPDATE_SUBMODULES=0 \
drivers/monado_driver/scripts/linux/install.sh
```

Если зависимости ещё не установлены, сначала выполнить:

```bash
XR_BUILD_ONLY=monado_driver ./devices/common/linux/scripts/build/install_build_deps_ubuntu.sh
```

Установщик применяет оба upstream-патча и включает поддержку futures.
Простого копирования `xr_monado_driver.cpp` в старую сборку недостаточно.
Для нового TCP-вывода геометрии/видео требуется также пересобрать
`xr_runtime_adapter` обычным способом. При SHM достаточно существующих
совместимых потоков `runtime_spatial_proxy_mesh` / `runtime_stereo_video`.

## Настройки нужно передать и сервису, и приложению

Настройки общие для железа:

```text
bin/drivers/monado_driver/profiles/runtime_data.env
```

`start.sh` загружает этот файл для сервиса. Helper `openxr_runtime_env.sh`
загружает тот же файл для OpenXR-приложения. Можно выбрать другой файл через
`XR_MONADO_DATA_CONFIG=/path/runtime_data.env`. Явные переменные окружения имеют
приоритет в поставляемом профиле. Если приложение запускается через Steam,
эти переменные должны быть в окружении процесса Steam либо в launch options.

Пример SHM в двух терминалах (потоки XR Gate уже запущены):

```bash
# Терминал сервиса
cd ~/xr-gate-release/xr-gate
export XR_MONADO_SPATIAL=1 XR_MONADO_VIDEO=1
bin/drivers/monado_driver/start.sh
```

```bash
# Терминал приложения
cd ~/xr-gate-release/xr-gate
export XR_MONADO_SPATIAL=1 XR_MONADO_VIDEO=1
source bin/drivers/monado_driver/openxr_runtime_env.sh
/path/to/openxr_application
```

Бэкенды должны быть включены отдельно. При настройке runtime используются:

```text
--spatial-proxy-mesh-input shm
--publish-runtime-spatial-proxy-mesh-shm
--video-input shm
--publish-runtime-video-shm
```

Сохраняйте остальные аргументы своего действующего профиля runtime.
Сначала проверьте доступность этих потоков и их format/frame_id в runtime registry.
Нормализованная геометрия для standard spatial entities должна иметь
`frame_id=runtime_local`. Camera-relative или другие исходные frames можно получать
как raw; не следует выдавать их за геометрию в tracking world.

## TCP

Для HMD, контроллеров, рук и mesh используется существующий application TCP сервер
runtime. В его аргументы добавить:

```text
--app-tcp
--app-tcp-host 127.0.0.1
--app-tcp-port 45810
--app-tcp-spatial
```

Spatial input у runtime должен быть включён. Mesh отправляется только при обновлении,
а также новому подключившемуся клиенту. Два bounded latest-message queues чередуют
tracking и mesh. При потере связи клиент очищает состояние, переподключается и
устанавливает новую исходную точку счётчиков кнопок. Часы разных компьютеров/VM
не сравниваются напрямую: для tracking/mesh используется возраст на сервере плюс
время после приёма.

`--app-tcp-spatial` по умолчанию выключен. Он добавляет сообщения `spatial_mesh`
к протоколу `xr_gate_app_v1`; старые TCP-клиенты, которые принимают только `frame`,
требуют обновления. В патче обновлён общий клиент SteamVR/Monado. Если одновременно
используется ранее собранный SteamVR-драйвер на этом endpoint, его тоже нужно
пересобрать либо оставить spatial TCP выключенным на его сервере.

В shell-launcher runtime эти настройки включаются переменными
`RUNTIME_APP_TCP=1`, `RUNTIME_APP_TCP_SPATIAL=1` и
`PUBLISH_RUNTIME_VIDEO_TCP=1`. Адрес и порт видео:
`RUNTIME_VIDEO_TCP_HOST` / `RUNTIME_VIDEO_TCP_PORT`. Их можно передать через
окружение соответствующего сервиса в конфигурации `xr_client`.

Видеоданные имеют отдельный бинарный endpoint, чтобы большие кадры не блокировали
tracking TCP. Runtime может пересылать поток `xr_video` с аргументами:

```text
--video-input shm
--publish-runtime-video-tcp
--runtime-video-tcp-host 127.0.0.1
--runtime-video-tcp-port 45812
```

Либо можно подключиться непосредственно к уже существующему TCP-серверу
`xr_video` (обычно порт 45700). Monado использует совместимый `XR_STEREO_VIDEO_V1`.

Настройки Monado и приложения:

```bash
export XR_MONADO_TRANSPORT=tcp
export XR_MONADO_TCP_HOST=127.0.0.1
export XR_MONADO_TCP_PORT=45810
export XR_MONADO_SPATIAL=1
export XR_MONADO_VIDEO=1
export XR_MONADO_VIDEO_TRANSPORT=tcp
export XR_MONADO_VIDEO_TCP_HOST=127.0.0.1
export XR_MONADO_VIDEO_TCP_PORT=45812
```

Для другого компьютера адреса server bind/client host задаются явно. Один worker
соединения tracking разделяется читателями в каждом процессе; Monado service и
OpenXR-приложение являются отдельными процессами. TCP видео переподключается
независимо. Его существующий wire format не содержит «server now», поэтому
проверяется срок после приёма; точный end-to-end age через разные часы не выводится.

## Потеря позы и recenter

Сброс определяется по входному `runtime_hmd_pose`: была невалидная/устаревшая/
недоступная поза, затем пришла валидная. Смена `reset_counter` или большой скачок
при непрерывно валидной позе сами по себе сбросом не считаются. Первое появление
валидного трекинга также завершает начальный период невалидности.

Переход отслеживает worker, поэтому потеря связи учитывается и между вызовами
приложения. На ближайшем `xrWaitFrame` вызывается native recenter Monado для
LOCAL / LOCAL_FLOOR. Он согласует HMD, руки, контроллеры и spatial и использует
обычные события изменения reference space. STAGE сохраняет семантику Monado.
Identity fallback драйвера не принимается за настоящую валидную входную позу.

Чтобы отключить автоматический recenter:

```bash
export XR_MONADO_RECENTER_ON_REACQUIRE=0
```

SHM geometry/video readers переоткрывают удалённые, изменившиеся и остановившиеся
объекты. Копирование через `pread` с повторной проверкой seqlock не обращается к
усечённому mmap. Старые timestamps не обновляются чтением; устаревшие данные
перестают выдаваться как свежие. При смене tracking epoch прежняя геометрия не
считается новой картой.

Основные дополнительные настройки:

| Переменная | По умолчанию |
|---|---|
| `XR_MONADO_TRANSPORT` | `shm` |
| `XR_MONADO_SPATIAL` / `XR_MONADO_VIDEO` | `0` / `0` |
| `XR_MONADO_RECENTER_ON_REACQUIRE` | `1` |
| `XR_MONADO_TCP_HOST` / `XR_MONADO_TCP_PORT` | `127.0.0.1` / `45810` |
| `XR_MONADO_TCP_RECONNECT_MS` / `XR_MONADO_TCP_TIMEOUT_MS` | `500` / `1000` |
| `XR_MONADO_SPATIAL_TRANSPORT` | транспорт tracking |
| `XR_MONADO_SPATIAL_REGISTRY` / `XR_MONADO_SPATIAL_STREAM` | `/tmp/runtime_tracking_streams.json` / `runtime_spatial_proxy_mesh` |
| `XR_MONADO_VIDEO_REGISTRY` / `XR_MONADO_VIDEO_STREAM` | `/tmp/runtime_video_streams.json` / `runtime_stereo_video` |
| `XR_MONADO_SPATIAL_MAX_AGE_MS` / `XR_MONADO_VIDEO_MAX_AGE_MS` | `1500` / `250` |
| `XR_MONADO_VIDEO_TRANSPORT` | `shm` |
| `XR_MONADO_VIDEO_TCP_HOST` / `XR_MONADO_VIDEO_TCP_PORT` | `127.0.0.1` / `45812` |

Прямые имена SHM можно задать через `XR_MONADO_HMD_SHM`,
`XR_MONADO_CONTROLLER_SHM`, `XR_MONADO_HAND_SHM`, `XR_MONADO_SPATIAL_SHM`,
`XR_MONADO_VIDEO_SHM`. При явном имени mesh caller обязан указать его frame
через `XR_MONADO_SPATIAL_FRAME`, если это не `runtime_local`.

## Проверки

```bash
MONADO_DIR="$PWD/third_party/monado_driver" \
  bash drivers/monado_driver/tests/run_tests.sh
MONADO_DIR="$PWD/third_party/monado_driver" \
  bash drivers/monado_driver/tests/run_data_tests.sh
```

`SANITIZE=1` включает AddressSanitizer/UndefinedBehaviorSanitizer.
Новые тесты используют настоящие SHM publishers и TCP servers, проверяют
lifecycle/snapshots/futures, raw payload, буферы/индексы, преобразование в baseSpace,
невалидная→валидная HMD, expiry и восстановление tracking/video TCP.
Monado C adapter компилируется с настоящими pinned заголовками; функциональные
тесты подменяют только проверку OpenXR handles и трансформацию пространства.

Пример приложения: `examples/xr_gate_data_probe.cpp`. Это headless-потребитель,
показывающий размеры raw-кадров и число entities; он не выводит видео на экран.
Тестирование изображения на реальных очках и приложения с mesh tracking остаётся
отдельной проверкой после установки.
