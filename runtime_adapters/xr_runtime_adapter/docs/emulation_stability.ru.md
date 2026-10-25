# Стабилизация кисти и отдельная калибровка Grip

Обновление применяется поверх full2. Режим эмуляции по-прежнему включается
`EMULATION_CONTROLLER=1`. Задержка подтверждения Trigger остаётся прежней — 45 мс.

## Постоянный профиль

Рабочая калибровка находится по пути:

```text
runtime_adapters/xr_runtime_adapter/configs/emulation-controller.json
```

В приложенном файле сохранена калибровка обеих рук из full2 и включён угловой
фильтр 2°. Обычный launcher автоматически выбирает этот файл относительно
`ROOT_PROJECT`, если `EMULATION_CONTROLLER_CONFIG` не задан. Явный путь в этой
переменной имеет приоритет. В консоли адаптера печатаются фактически выбранный
профиль и порог фильтра. В собранных релизах файл размещается по тому же пути.

## Установка в существующий релиз

В примерах исходники — `~/src/xr-gate`, релиз — `~/xr-gate-release/xr-gate`,
пакет распакован в `~/xr-gate-emulation-wrist-grip`. Замени пути на свои.

1. Останови трекинг. Из папки исходников примени один новый патч:

```bash
cd "$HOME/src/xr-gate"
git apply --check "$HOME/Downloads/xr-gate-emulation-wrist-grip-v2.patch"
git apply "$HOME/Downloads/xr-gate-emulation-wrist-grip-v2.patch"
mkdir -p runtime_adapters/xr_runtime_adapter/configs
cp "$HOME/xr-gate-emulation-wrist-grip/runtime_adapters/xr_runtime_adapter/configs/emulation-controller.json" \
  runtime_adapters/xr_runtime_adapter/configs/emulation-controller.json
```

2. Пересобери адаптер и инструмент калибровки для используемого релиза:

```bash
ROOT_PROJECT="$PWD" BUILD_JOBS=2 \
  INSTALL_BIN_DIR="$HOME/xr-gate-release/xr-gate/bin/runtime_adapters/xr_runtime_adapter" \
  INSTALL_SCRIPT_DIR="$HOME/xr-gate-release/xr-gate/bin/scripts/runtime_adapters/xr_runtime_adapter" \
  bash runtime_adapters/xr_runtime_adapter/scripts/linux/install_xr_runtime_adapter.sh
```

Если запускаешь из исходников, убери `INSTALL_BIN_DIR` и `INSTALL_SCRIPT_DIR`.
Basalt, Mercury и драйверы SteamVR/Monado пересобирать не требуется.

3. Для уже распакованного релиза скопируй профиль в указанную папку:

```bash
mkdir -p "$HOME/xr-gate-release/xr-gate/runtime_adapters/xr_runtime_adapter/configs"
cp runtime_adapters/xr_runtime_adapter/configs/emulation-controller.json \
  "$HOME/xr-gate-release/xr-gate/runtime_adapters/xr_runtime_adapter/configs/emulation-controller.json"
```

4. Запусти из папки релиза:

```bash
cd "$HOME/xr-gate-release/xr-gate"
EMULATION_CONTROLLER=1 RUNTIME_APP_TCP=1 ./run_xr_client.sh --config xreal_ultra
```

Сохрани свой профиль устройства `--config`, если он другой. Если ранее
экспортировал `EMULATION_CONTROLLER_CONFIG` со старым путём, перед запуском
выполни `unset EMULATION_CONTROLLER_CONFIG` либо укажи новый путь явно.

## Угловой фильтр

В рабочем JSON доступны:

```json
"wrist_filter_enabled": true,
"wrist_filter_degrees": 2
```

Общий поворот кисти удерживается в пределах выбранного углового порога.
При дальнейшем движении ориентация следует за кистью плавно, с угловым
отставанием до заданного порога. Одна и та же коррекция вокруг запястья
применяется к контроллеру, ладони, запястью, позициям и ориентациям всех 21
суставов. Сгибы пальцев и перемещение руки сохраняются; дополнительного
временного подтверждения жестов фильтр не вводит.

Фильтр действует только в новом режиме эмуляции, на включённых сторонах.
При свежем физическом контроллере на стороне или блокировке жестов по потоку
override фильтр этой стороны отключается. Смена источника/начала координат
и потеря трекинга сбрасывают его опорную ориентацию.

Для отключения поставь `wrist_filter_enabled: false` или
`wrist_filter_degrees: 0`. После изменения JSON перезапусти трекинг.
Порог можно уменьшить до 1° или увеличить, например до 3°. Настройка
`OVERRIDE_CONTROLLER_BLOCK_GESTURES_WHILE_STREAM_PRESENT` сохраняется.

## Отдельная запись Grip

Теперь Grip может иметь собственную калибровку мизинца для каждой руки.
Она не меняет оси, обычную классификацию пальцев для A/B/V/Menu и Trigger.
Для записи доступны 20 этапов: согнутый/разогнутый мизинец, прямая кисть,
наклоны вперёд/назад, большой отогнут и прижат.

Из папки распакованного пакета:

```bash
python3 xr_gate_gesture_debug.py --grip --prepare 3 --hold 4 \
  --profile "$HOME/xr-gate-release/xr-gate/runtime_adapters/xr_runtime_adapter/configs/emulation-controller.json" \
  --output "$HOME/xr-gate-grip.zip"
```

Скрипт также обновлён в исходниках. В новом полном релизе можно использовать
`./gesture_debug.sh --grip --prepare 3 --hold 4`. Для одной руки добавь
`--hand left` или `--hand right`.

Перед записью останови `override_controller` либо временно установи его
блокировку жестов в 0. Мизинец должен быть виден камерам; указательный,
средний и безымянный на всех этапах остаются согнутыми. Вторую руку держи
спокойно. После записи пришли `xr-gate-grip.zip` для проверки суставов и
построения нового профиля Grip.

## Самостоятельный расчёт Grip

Если хочешь рассчитать профиль локально, извлеки `frames.jsonl` из ZIP в
отдельную папку и запусти:

```bash
./bin/runtime_adapters/xr_runtime_adapter/xr_emulation_tool calibrate-grip \
  /путь/к/frames.jsonl /путь/к/emulation-controller-grip.json \
  --config "$PWD/runtime_adapters/xr_runtime_adapter/configs/emulation-controller.json"
```

Инструмент сохраняет новый файл, добавляя только `left.grip_calibration`
и/или `right.grip_calibration`; все остальные поля сохраняются. Для каждой
позы нужны минимум 10 достоверных разных оптических кадров.
Если трекер выдаёт одинаково согнутый мизинец в обеих позах, расчёт
отклоняется с сообщением `Grip open/closed overlap`; выходной файл не создаётся.
После успешного расчёта новый JSON можно скопировать на место рабочего
`configs/emulation-controller.json` и перезапустить трекинг.

Проверены 13 тестовых наборов, включая реальный TCP-выход адаптера, все кости,
сохранение сгибов и перемещения руки, отключение фильтра для физического
контроллера, загрузку профиля из configs, сохранение остальных настроек при
докалибровке Grip и отказ при неразличимых позах.
На ранее присланной записи воспроизведены 55 007 кадров с новым фильтром 2°:
оси на всех 12 этапах нейтрали остаются нулевыми, направления D-pad и специальные
жесты сохраняются. Это проверка на записанных данных; удобство наведения
предстоит оценить в живом запуске.
