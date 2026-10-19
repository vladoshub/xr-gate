# XR Gate: установка в Windows через WSL2

[English](START-HERE.html) · [Русский](START-HERE.ru.html)

## 1. Подготовь Windows

1. Используй Windows 11 x64 и процессор с поддержкой AVX2.
2. Установи Steam и SteamVR. Один раз открой SteamVR, затем закрой его.
3. Распакуй весь архив в постоянную папку, например C:\XR-Gate-WSL. Сохрани подпапки linux и windows рядом с XrGate-Wsl.ps1.
4. Открой обычный Windows PowerShell в этой папке. Все команды ниже выполняй в Windows.

## 2. Установи XR Gate

1. Дважды щёлкни INSTALL.cmd или выполни:

~~~powershell
.\INSTALL.cmd
~~~

Для контроллеров Gear VR используй эту команду:

~~~powershell
.\INSTALL.cmd -GearVR
~~~

2. Подтверди запрос прав администратора Windows.
3. Если установщик запросит перезагрузку Windows, сохрани работу, перезагрузи компьютер и повтори команду установки.
4. Если откроется окно Ubuntu, создай имя Linux-пользователя и пароль. В командной строке Ubuntu введи exit, затем повтори команду установки.
5. Если установщик попросит перезапустить Ubuntu после включения systemd, сохрани работу в Ubuntu и выполни:

~~~powershell
wsl --terminate Ubuntu-24.04
.\INSTALL.cmd
~~~

Повтори свои параметры установки, например -GearVR. Код завершения 20 означает, что нужно выполнить указанное действие и повторить установку.

6. Если регистрация драйвера была отложена, закрой SteamVR и выполни:

~~~powershell
.\DRIVER.cmd
~~~

Если SteamVR не найден, укажи папку его установки:

~~~powershell
.\DRIVER.cmd -SteamVR 'D:\SteamLibrary\steamapps\common\SteamVR'
~~~

## 3. Подготовь очки (на примере очков XREAL ULTRA)


1. Подключи видеоканал XREAL Air 2 Ultra к видеокарте Windows через USB-C с видеовыходом или питаемый адаптер DP/HDMI → USB-C.
2. Надень очки или активируй их датчик присутствия.
3. Удерживай Brightness Up (+) примерно три секунды до сигнала, чтобы включить полный SBS.
4. В Windows открой Параметры → Система → Дисплей и выбери «Расширить эти экраны». Выбери очки и установи 3840×1080 и 60 Hz.
5. Нажми «Определить» и запомни номер, который появится в очках. Если очков нет среди экранов Windows, открой настройки разработчика SteamVR, нажми «Выключить прямой режим», затем закрой SteamVR и повтори шаг 4.
6. При закрытом SteamVR дважды щёлкни DISPLAY.cmd или выполни:

~~~powershell
.\DISPLAY.cmd
~~~

7. Выбери строку списка с экраном очков. Сопоставь номер DISPLAY с кнопкой «Определить» в Windows; для различения экранов используй также название. Команда сохранит выбор и настроит вывод в расширенном SBS.
8. Если используешь Gear VR, выполни раздел 5 перед запуском.

## 4. Запусти XR Gate и SteamVR

1. Подключи очки и включи SBS. Контроллеры с IMU положи на ровную поверхность и держи неподвижно во время начальной калибровки.
2. Дважды щёлкни RUN.cmd или выполни:

~~~powershell
.\RUN.cmd
~~~

3. В консоли xr_client пройди стартовые подсказки и калибровку. Не закрывай камеры; обеспечь хорошее освещение. При запуске трекинга несколько секунд плавно двигай головой.
4. Дождись открытия SteamVR.
5. Для этой настройки оставь прямой режим SteamVR выключенным.
6. Если XR Gate заблокирован, открой Settings → Startup/Shutdown → Manage Add-ons и включи его.
7. Проверь, что в очках появилась сцена SteamVR и она реагирует на движение головы, затем запускай игру.

Для следующих запусков включи SBS и запусти RUN.cmd. Команда повторно применит сохранённый выбор экрана с его текущими координатами рабочего стола. После смены кабеля/порта или если сохранённый экран не найден, закрой SteamVR и снова запусти DISPLAY.cmd.

Чтобы запустить только xr_client:

~~~powershell
.\CONSOLE.cmd
~~~

Если настройка занимает больше 180 секунд, закончи действия в открытом окне xr_client и снова запусти RUN.cmd. Чтобы увеличить ожидание:

~~~powershell
.\RUN.cmd -TimeoutSeconds 300
~~~

Если очки нужно выбрать вручную:

~~~powershell
usbipd list
.\USB.cmd -XrealBusId 1-2
~~~

Замени 1-2 на BUSID очков из usbipd list.

## 5. Настрой контроллеры Gear VR

1. Подключи очки и выделенный USB Bluetooth-донгл. Если при установке Gear VR не был включён, выполни:

~~~powershell
.\INSTALL.cmd -GearVR
~~~

2. Найди BUSID донгла:

~~~powershell
usbipd list
~~~

3. Подключи донгл к WSL:

~~~powershell
.\USB.cmd -BluetoothBusId 1-4
~~~

Замени 1-4 на BUSID своего донгла.

4. Открой консоль сопряжения Bluetooth:

~~~powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\XrGate-Wsl.ps1 -Action Pair
~~~

5. Переведи контроллеры в режим сопряжения и введи команды в bluetoothctl. Вместо MAC_LEFT и MAC_RIGHT укажи реальные адреса контроллеров:

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

6. Запусти мастер регистрации контроллеров:

~~~powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\XrGate-Wsl.ps1 -Action Controllers -GearVR
~~~

7. По подсказкам назначь левый и правый контроллеры и сохрани конфигурацию.
8. Запусти RUN.cmd.

Если уже есть конфигурация gearvr.json, импортируй её вместо повторной регистрации:

~~~powershell
.\INSTALL.cmd -GearVR -ControllerConfig 'C:\XR-Gate-WSL\gearvr.json'
~~~

Перед запуском XR Gate выполни сопряжение Bluetooth. Для другого USB Bluetooth-донгла повтори шаг 3 с его BUSID.

## 6. Останови XR Gate

1. Закрой SteamVR.
2. Нажми Ctrl+C в консоли xr_client и дождись остановки.
3. Верни выбранные USB-устройства Windows:

~~~powershell
.\DETACH.cmd
~~~

4. Чтобы использовать очки как обычный дисплей, переключи их в 2D аппаратной кнопкой.

## 7. Измени настройки или обнови XR Gate

### Обновление скриптов установки

1. Закрой SteamVR.
2. Распакуй новый архив в ту же папку установки и замени файлы, включая подпапки linux и windows.
3. Включи SBS и выбери соответствующие разрешение и частоту в Windows.
4. Один раз запусти DISPLAY.cmd и выбери экран очков.
5. Запусти RUN.cmd.

### Обновление XR Gate

1. Закрой SteamVR и останови xr_client.
2. Повтори INSTALL.cmd.
3. Включи SBS, выбери соответствующую частоту в Windows и запусти RUN.cmd. Сохранённый выбор экрана применится к новому драйверу. Если экран больше не найден, закрой SteamVR и снова запусти DISPLAY.cmd.

Для установки определённого релиза:

~~~powershell
.\INSTALL.cmd -ReleaseTag v2.8.0
~~~

### Переключение на 90 Hz

1. Закрой SteamVR и останови xr_client.
2. Установи соответствующий драйвер:

~~~powershell
.\INSTALL.cmd -FrequencyHz 90 -DisplayMode extended_sbs
~~~

3. Включи SBS/90 Hz на очках и выбери 90 Hz в параметрах дисплея Windows.
4. Запусти DISPLAY.cmd, затем RUN.cmd.

### Использование расширенного SBS-дисплея

1. Если прямой режим включён, открой настройки разработчика SteamVR и нажми «Выключить прямой режим». Закрой SteamVR; xr_client можно оставить работающим.
2. В Windows выбери «Расширить эти экраны», разрешение 3840×1080 и частоту, соответствующую установленному драйверу.
3. Выполни:

~~~powershell
.\DISPLAY.cmd
~~~

4. Выбери экран очков в списке, затем запусти RUN.cmd.

### Использование прямого режима

1. Закрой SteamVR и останови xr_client.
2. Установи пакет для прямого режима:

~~~powershell
.\INSTALL.cmd -FrequencyHz 60 -DisplayMode direct
~~~

3. Запусти XR Gate и SteamVR, нажми «Включить прямой режим» в настройках разработчика SteamVR и перезапусти SteamVR.
4. Если SteamVR показывает ошибку 422 или в очках остаётся рабочий стол Windows, выполни шаги из раздела «Использование расширенного SBS-дисплея» выше.

### Использование своего профиля xr_client

1. Помести профиль в ~/xr-gate-wsl/config/xr_client.json внутри Ubuntu.
2. Останови xr_client и повтори RUN.cmd.

## 8. Устрани проблемы запуска

### Ошибка SteamVR 422 или рабочий стол Windows в очках

1. Открой настройки разработчика SteamVR и нажми «Выключить прямой режим». Закрой SteamVR; xr_client оставь открытым.
2. Включи SBS. В параметрах дисплея Windows выбери «Расширить эти экраны», разрешение 3840×1080 и частоту, соответствующую установленному драйверу.
3. Нажми «Определить», затем выполни:

~~~powershell
.\DISPLAY.cmd
.\RUN.cmd
~~~

4. При запросе выбери экран очков. Дождись запуска SteamVR и проверь движение головы в его сцене.
5. Если ошибка 422 повторится, закрой SteamVR, повтори DISPLAY.cmd для проверки выбранного экрана и посмотри свежий журнал композитора:

~~~powershell
$vrPaths = Get-Content "$env:LOCALAPPDATA\openvr\openvrpaths.vrpath" -Raw | ConvertFrom-Json
$steamFolder = Split-Path -Parent $vrPaths.config[0]
Get-Content (Join-Path $steamFolder 'logs\vrcompositor.txt') -Tail 100
~~~

### Просмотр диагностики

1. Выполни:

~~~powershell
.\DOCTOR.cmd
~~~

2. При работающем xr_client проверь соединение с трекингом:

~~~powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\XrGate-Wsl.ps1 -Action Test -TimeoutSeconds 30
~~~

### Переподключение камер или Bluetooth

1. Закрой SteamVR и останови xr_client.
2. Переподключи USB-кабель или донгл.
3. Запусти USB.cmd, затем RUN.cmd.
4. Если устройство по-прежнему отсутствует, выполни:

~~~powershell
wsl -d Ubuntu-24.04 -- lsusb
wsl -d Ubuntu-24.04 -- uname -r
wsl -d Ubuntu-24.04 -u root -- modprobe uvcvideo
wsl -d Ubuntu-24.04 -u root -- modprobe btusb
wsl -d Ubuntu-24.04 -- bluetoothctl list
wsl -d Ubuntu-24.04 -u root -- dmesg
~~~

5. Если модуль не найден, обнови WSL:

~~~powershell
wsl --update --web-download
~~~

6. Сохрани работу в Ubuntu, перезапусти дистрибутив и подключи устройства:

~~~powershell
wsl --terminate Ubuntu-24.04
.\USB.cmd
.\RUN.cmd
~~~

Если в %USERPROFILE%\.wslconfig указано собственное ядро, удали или обнови эту настройку перед повторением шагов.

### Восстановление соединения из Windows

1. Оставь xr_client работающим и выполни:

~~~powershell
Test-NetConnection 127.0.0.1 -Port 45810
wsl -d Ubuntu-24.04 -- ss -ltnp
~~~

2. Для сети WSL NAT включи localhost forwarding в %USERPROFILE%\.wslconfig, сохранив остальные настройки:

~~~ini
[wsl2]
localhostForwarding=true
~~~

3. Сохрани работу во всех дистрибутивах WSL и выполни:

~~~powershell
wsl --shutdown
.\RUN.cmd
~~~

4. Если соединения по-прежнему нет, проверь настройки VPN/брандмауэра и закрой другие процессы, использующие порт 45810.

### Создание обычного пользователя Ubuntu

Если Ubuntu запускается от root:

1. Открой Ubuntu и создай пользователя. Вместо xruser укажи желаемое имя Linux-пользователя:

~~~bash
adduser xruser
usermod -aG sudo xruser
~~~

2. Укажи пользователя в /etc/wsl.conf, сохранив остальные секции:

~~~ini
[user]
default=xruser
~~~

3. Сохрани работу в Ubuntu, затем выполни из Windows:

~~~powershell
wsl --terminate Ubuntu-24.04
.\INSTALL.cmd
~~~

### Установка недостающего компонента Windows

1. Если нет winget, установи или обнови Microsoft App Installer и повтори INSTALL.cmd.
2. Если Windows сообщает об отключённой виртуализации, включи SVM/AMD-V или VT-x в BIOS/UEFI, перезагрузи компьютер и повтори INSTALL.cmd.

