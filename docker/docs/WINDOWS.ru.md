# Windows: WSL + Docker + XREAL + USB Bluetooth

Целевой вариант: Windows 11 x64, современный WSL, Ubuntu 24.04, Docker Engine внутри этой Ubuntu. Бинарники XR Gate остаются Linux-бинарниками из релиза. TCP-клиент работает в Windows. Windows SteamVR-драйвер скачивается из отдельного CI artifact; инструкция — STEAMVR.ru.md.

## Что с WSL 3

Проверено **16 октября 2026**: последняя стабильная версия пакета Microsoft WSL — **3.0.3**, выпущена 9 октября, ядро в релизе — **6.18.54.1**. Это версия WSL runtime; дистрибутив Ubuntu по-прежнему настраивается как **WSL 2**: `wsl --set-version Ubuntu-24.04 2`. Команда с `3` здесь не нужна.

В текущей ветке ядра `linux-msft-wsl-6.18.y` включены `CONFIG_USB_VIDEO_CLASS=m`, `CONFIG_HIDRAW=y`, `CONFIG_BT=m`, `CONFIG_BT_HCIBTUSB=m`. Это позволяет пробовать UVC/HID/USB Bluetooth со штатным современным ядром, но конкретный адаптер всё равно требует подходящих модулей/firmware и проверки на машине.

У Microsoft также есть `wslc.exe` — WSL Containers. В этом комплекте выбран обычный Docker Engine внутри Ubuntu, где доступны USB-узлы, systemd и BlueZ. USB-сценарий через отдельный `wslc` здесь не реализован и не проверен.

## 1. Установить WSL и Ubuntu

В PowerShell **от администратора**:

```powershell
wsl --install --no-distribution
wsl --update --web-download
winget install --exact --id dorssel.usbipd-win
```

При необходимости сохраните работу и перезагрузите Windows. Затем в **обычном** PowerShell под пользователем, который будет запускать XR Gate:

```powershell
wsl --set-default-version 2
wsl --install -d Ubuntu-24.04 --web-download
```

Создайте Linux-пользователя и пароль; символы пароля при вводе не отображаются. Не создавайте Ubuntu под другой учётной записью администратора.

Скопируйте целиком папку **docker/** из репозитория в постоянную Windows-папку. Для запуска helper-скриптов в открытом PowerShell можно разрешить их только в этом процессе:

```powershell
Set-ExecutionPolicy -Scope Process Bypass
```

В существующей Ubuntu той же Windows-учётной записи `windows/Setup-Windows.ps1` доступен как дополнительный elevated helper обновления WSL/usbipd. Сам он права администратора не запрашивает автоматически.

## 2. Подготовить Ubuntu и Docker Engine

В обычном PowerShell из каталога комплекта:

```powershell
.\windows\Install-XrGate.ps1
```

Файлы копируются в `$HOME/xr-gate-docker` внутри Linux-файловой системы Ubuntu. Скрипт устанавливает Docker Engine, Compose, USB-инструменты, BlueZ/firmware; затем собирает образ из готового релиза и запускает software-only проверку образа. Сборки исходников XR Gate нет.

Если systemd в Ubuntu был выключен, установщик включит его и завершится с кодом **20**. Сохрани работу в этой Ubuntu, затем выполни в Windows и повтори установщик:

```powershell
wsl --terminate Ubuntu-24.04
.\windows\Install-XrGate.ps1
```

Остановка дистрибутива завершает все его текущие процессы. Скрипт не делает её автоматически.

Для другого имени установленной Ubuntu 24.04 или другого каталога передавай одинаковые параметры всем скриптам:

```powershell
.\windows\Install-XrGate.ps1 -Distribution Ubuntu-24.04 -LinuxPath /home/me/xr-gate-docker
.\windows\XrGate.ps1 -Command Doctor -Distribution Ubuntu-24.04 -LinuxPath /home/me/xr-gate-docker
```

Хранить активные файлы и логи под `/mnt/c` не рекомендуется. Их можно открыть из Windows через `\\wsl.localhost\Ubuntu-24.04\home\<user>\xr-gate-docker`. Скрипты используют root в выбранной Ubuntu для локального Docker/USB; Windows administrator для обычного запуска не требуется.

Docker Desktop не нужен. Если он уже есть, отключи его WSL integration для выбранной Ubuntu и используй установленный внутри неё Engine. Скрипт не удаляет существующие контейнерные среды. При конфликте distro-пакетов с Docker CE он выдаст инструкцию, вместо автоматического удаления.

## 3. Передать XREAL и USB Bluetooth в WSL

Узнай **BUSID** обоих устройств:

```powershell
.\windows\Usb-XrGate.ps1 -Action List
```

У XREAL Air 2 Ultra ожидается VID:PID `3318:0426`. USB Bluetooth выбирай по своему адаптеру. Следующие BUSID — только пример, замени их фактическими.

Один раз, в PowerShell **от администратора** (при необходимости выполните в этом окне Set-ExecutionPolicy -Scope Process Bypass):

```powershell
.\windows\Usb-XrGate.ps1 -Action Bind -BusId '1-2','1-4'
```

Открой Ubuntu в отдельном терминале и оставь его открытым. Затем в обычном PowerShell:

```powershell
.\windows\Usb-XrGate.ps1 -Action Attach -BusId '1-2','1-4'
.\windows\XrGate.ps1 -Command Doctor -Bluetooth
```

При attach USB-устройство переходит в Linux и временно недоступно Windows. Поэтому используй отдельный USB Bluetooth dongle, если встроенный Bluetooth нужен Windows. USB-проброс не передаёт DisplayPort Alt Mode: подключение дисплея очков к Windows нужно проверять отдельно, это не видеоканал приложения TCP.

После сна, переподключения или изменения BUSID повтори `List`/`Attach`, затем останови и заново запусти контейнер. Bind обычно сохраняется; attach после перезапуска WSL может потребовать повторения.

## 4. Запустить с консолью

```powershell
# XREAL, без физических контроллеров:
.\windows\XrGate.ps1 -Command Run

# С доступом к USB Bluetooth через host BlueZ:
.\windows\XrGate.ps1 -Command Run -Bluetooth
```

Это интерактивная консоль `xr_client`, включая меню и управляющие команды. Выбери режим дисплея и выполни startup gate. Для сохранения текущего display mode:

```powershell
.\windows\XrGate.ps1 -Command Run -ClientArguments @('--prestart-option','current')
```

Если нужен фон и отдельное подключение консоли:

```powershell
.\windows\XrGate.ps1 -Command Up -Bluetooth
.\windows\XrGate.ps1 -Command Console
```

Отсоединение без остановки — **Ctrl+P, Ctrl+Q**. Остановка:

```powershell
.\windows\XrGate.ps1 -Command Stop
```

## 5. Проверить TCP из Windows

В другом PowerShell, когда `runtime_adapter` уже запущен:

```powershell
.\windows\Test-XrGate.ps1
```

Windows-приложение подключается к **127.0.0.1:45810** и читает JSON Lines. Docker публикует этот порт только на loopback Ubuntu; WSL обеспечивает доступ из Windows через localhost. IPv4 `127.0.0.1` указан явно, чтобы избежать различий разрешения имени `localhost` в IPv6.

Комплект не создаёт LAN-forwarding, firewall allow-rules или `netsh portproxy` на `0.0.0.0`. Оставь штатный localhost forwarding WSL включённым. Если тест в Ubuntu проходит, а в Windows нет, проверь `%UserProfile%\.wslconfig`, сторонний firewall/VPN и доступность того же порта в Windows. Менять Docker bind на все интерфейсы для исправления не требуется.

Для проверки фактических Docker bindings:

```powershell
wsl -d Ubuntu-24.04 -u root -- docker port xr-gate
```

Ожидается `45810/tcp -> 127.0.0.1:45810`. При отдельном `-Video` также будет `45700/tcp -> 127.0.0.1:45700`.

## 6. Конфиги и Bluetooth-контроллеры

```powershell
.\windows\XrGate.ps1 -Command ExportConfigs
```

Экспорт появится в Linux-каталоге `exported-configs/`. Помести нужные файлы в Linux-каталог `config/`:

- `xr_client.json` — твой вариант профиля из релиза;
- `override_controller.json` — обученные bindings/calibration контроллеров.

Путь можно открыть через `\\wsl.localhost\Ubuntu-24.04\...`. Редактирование первоначальной Windows-копии после установки не обновляет Linux-копию автоматически.

Сам флаг `-Bluetooth` только подключает BlueZ к контейнеру. Pair/trust, provider и обучение описаны в [CONTROLLERS.md](CONTROLLERS.md). Для evdev нужны явные Linux `/dev/input/eventN` в `.env`, а не Windows DirectInput/XInput устройства.

Отключить устройства от WSL и вернуть Windows, предварительно остановив контейнер:

```powershell
.\windows\Usb-XrGate.ps1 -Action Detach -BusId '1-2','1-4'
```

## Если нет камеры/HCI

В Ubuntu проверь:

```bash
uname -r
lsusb
ls -l /dev/video* /dev/hidraw*
sudo modprobe uvcvideo
sudo modprobe btusb
bluetoothctl list
rfkill list
sudo systemctl status bluetooth
```

Если модуль отсутствует, обнови WSL через `wsl --update`, проверь, не выбран ли старый custom kernel в `.wslconfig`, и перезапусти дистрибутив после сохранения работы. Если `btusb` загрузился, но адаптер не появился, проверь firmware и `dmesg`. Если HCI есть, но заблокирован: `sudo rfkill unblock bluetooth`, затем `bluetoothctl power on`.

Если в твоём ядре действительно нет нужного драйвера, понадобится поддерживаемое ядро с соответствующим модулем либо собственная сборка ядра по инструкции Microsoft. Docker-образ сам драйвер ядра не добавит. Физический XREAL и конкретный Bluetooth dongle в среде подготовки комплекта отсутствовали; end-to-end проверка на Windows ещё нужна.
