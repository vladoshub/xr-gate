# XR Gate в Windows (установка через WSL2)

[English](README.md) · [Русский](README.ru.md)

## 1. Скачайте и подготовьте

1. Нужны Windows 11 x64 и процессор с поддержкой AVX2.
2. Установите Steam и SteamVR. Один раз откройте SteamVR, затем закройте его.
3. Скачайте **xr-gate-windows.zip** из [Releases](https://github.com/vladoshub/xr-gate/releases) или artifact **xr-gate-windows** из своего запуска CI.
4. Распакуйте ZIP в постоянную папку. Откройте вложенную папку `xr-gate-windows`. Сохраните подпапки `drivers`, `hardware`, `linux` и `windows` вместе.
5. Откройте в этой папке обычный Windows PowerShell. Все команды ниже выполняйте в нём.

## 2. Настройте оборудование

Для XREAL Air 2 Ultra готовый профиль уже настроен. Один раз создайте файл своих настроек:

~~~powershell
if (!(Test-Path .env)) { Copy-Item .env.example .env }
~~~

Для другого устройства скопируйте универсальный шаблон:

~~~powershell
Copy-Item hardware\generic.env.example hardware\my-device.env
~~~

Отредактируйте `hardware/my-device.env`, затем укажите `XR_HARDWARE_ENV=hardware/my-device.env` в `.env`.

1. Укажите профиль запуска в `XR_PROFILE` и имя устройства в `XR_TARGET_DEVICE`.
2. Укажите VID:PID камер и IMU в `XR_USB_IDS`. Для ручного выбора выполните `usbipd list` и задайте `XR_USB_BUS_IDS` в `.env`. Для нескольких устройств разделите BUSID запятыми. Вместо них можно задать постоянные пути Windows USB instance в `XR_USB_INSTANCE_IDS`.
3. При необходимости задайте Linux-пути камеры, HID, serial и устройств ввода: `XR_CAMERA_DEVICE`, `XR_HIDRAW_DEVICES`, `XR_SERIAL_DEVICES`, `XR_INPUT_DEVICES`. Если профиль использует камеру, включите `XR_REQUIRE_CAMERA=1`.
4. Укажите подходящий скрипт настройки из релиза в `XR_DEVICE_ACCESS_SCRIPT` или оставьте поле пустым. Перечислите нужные модули Linux в `XR_KERNEL_MODULES`.
5. Задайте `XR_WINDOW_WIDTH`, `XR_WINDOW_HEIGHT`, `XR_RENDER_WIDTH`, `XR_RENDER_HEIGHT`, `XR_DISPLAY_FREQUENCY_HZ` и оптические параметры своего дисплея.

Чтобы переопределить сохранённый выбор, укажите настройку прямо в `.env`. Выбирайте частоту и `XR_DISPLAY_MODE` (`direct` или `extended_sbs`), которые есть в архиве. Для отдельного Bluetooth-донгла укажите его BUSID в `XR_BLUETOOTH_BUS_ID`. Для отключения сохранённого донгла укажите `XR_BLUETOOTH=0`; для отключения Gear VR — `XR_GEARVR=0`. Если SteamVR не найден автоматически, задайте `XR_STEAMVR_PATH`.

## 3. Установите

1. Дважды щёлкните **INSTALL.cmd** или выполните:

~~~powershell
.\INSTALL.cmd
~~~

Для контроллеров Gear VR используйте:

~~~powershell
.\INSTALL.cmd -GearVR
~~~

2. Подтвердите запрос прав администратора. Если запрошена перезагрузка, перезапустите Windows и повторите ту же команду.
3. Если Ubuntu открылась впервые, создайте имя Linux-пользователя и пароль, введите `exit` и повторите команду установки.
4. Если запрошен перезапуск Ubuntu, сохраните в ней работу и выполните:

~~~powershell
wsl --terminate Ubuntu-24.04
.\INSTALL.cmd
~~~

Если меняли `XR_DISTRIBUTION`, используйте указанное там имя дистрибутива. Повторите `-GearVR`, если использовали его.

5. Если регистрация драйвера отложена, закройте SteamVR и выполните:

~~~powershell
.\DRIVER.cmd
~~~

## 4. Выберите дисплей

Для **extended_sbs**:

1. Подключите видеовход очков к видеокарте Windows и включите SBS на очках.
2. В настройках дисплея Windows выберите **Расширить эти экраны**. Установите для очков разрешение и частоту из аппаратного env.
3. Отключите прямой режим SteamVR и закройте SteamVR.
4. Выполните:

~~~powershell
.\DISPLAY.cmd
~~~

5. Выберите строку очков по кнопке **Определить** в Windows. Чтобы закрепить имя выхода, укажите `XR_DISPLAY_NAME` в `.env` и повторите команду.

Для **direct** используйте `Direct-Mode-Guide.bat` в выбранной папке `drivers/<частота>Hz-direct/xr_tracking`.

## 5. Запустите

1. Подключите устройства. Положите IMU-контроллеры на ровную поверхность и не двигайте их.
2. Выполните:

~~~powershell
.\RUN.cmd
~~~

3. Пройдите стартовые подсказки и калибровку в открывшейся консоли. Оставьте её открытой и дождитесь SteamVR.
4. При запросе включите дополнение XR Gate в SteamVR. Проверьте гарнитуру и запустите игру.

Для следующего сеанса подключите устройства и снова запустите **RUN.cmd**. После смены порта дисплея закройте SteamVR и повторите **DISPLAY.cmd**.

Для запуска трекинга без открытия SteamVR:

~~~powershell
.\CONSOLE.cmd
~~~

## 6. Подключите контроллеры Gear VR

1. Укажите BUSID донгла в `XR_BLUETOOTH_BUS_ID` в `.env`, затем подключите устройства:

~~~powershell
.\USB.cmd
~~~

2. Откройте консоль сопряжения:

~~~powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\XrGate-Wsl.ps1 -Action Pair
~~~

3. Включите режим сопряжения на контроллерах. Подставьте их адреса вместо `MAC_LEFT` и `MAC_RIGHT` и введите:

~~~text
power on
agent on
default-agent
scan on
pair MAC_LEFT
trust MAC_LEFT
connect MAC_LEFT
pair MAC_RIGHT
trust MAC_RIGHT
connect MAC_RIGHT
scan off
quit
~~~

4. Если трекинг запущен, остановите его. Запустите регистрацию:

~~~powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\XrGate-Wsl.ps1 -Action Controllers -GearVR
~~~

5. Назначьте левый и правый контроллеры по подсказкам, сохраните конфиг и запустите **RUN.cmd**.

## 7. Остановите или измените настройки

1. Закройте SteamVR.
2. Нажмите **Ctrl+C** в консоли трекинга и дождитесь остановки.
3. Для возврата USB-устройств Windows выполните:

~~~powershell
.\DETACH.cmd
~~~

После изменения аппаратных настроек подключите устройства и запустите снова. После смены релиза, частоты драйвера или режима дисплея в `.env` повторите **INSTALL.cmd** и настройку дисплея.

## 8. Если не запускается

Выполните:

~~~powershell
.\DOCTOR.cmd
~~~

Не закрывая консоль трекинга, проверьте его:

~~~powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\XrGate-Wsl.ps1 -Action Test
~~~

Если SteamVR сообщает о неверном дисплее рабочего стола, закройте SteamVR, проверьте расширение экранов, разрешение и частоту в Windows, затем повторите **DISPLAY.cmd** и **RUN.cmd**.
