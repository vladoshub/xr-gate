# SteamVR-драйвер XR Gate в Windows

Русский · [English](STEAMVR.en.md)

Это SteamVR-драйвер для Windows x64. Capture и трекинг XR Gate работают в Linux,
WSL или Docker, SteamVR и игра — в Windows. Драйвер подключается к серверу
`xr_gate_app_v1` по TCP **127.0.0.1:45810**. Изображение поступает в XREAL через
видеоподключение к GPU Windows. Передача USB камер/HID через USB/IP не передаёт
DisplayPort-видео.

## 1. Скачать Windows-пакет

После применения сопровождающего патча и успешного запуска GitHub Actions:

1. Откройте **Actions → XR Gate split build** и успешную сборку изменённого commit.
2. Скачайте artifact **xr-gate-steamvr-windows-x64**. В Linux-релизе v2.3.0 этого
   нового Windows-драйвера нет.
3. Распакуйте artifact, затем **xr-gate-steamvr-windows-x64-60Hz-direct.zip**
   в постоянную папку, например `C:\XR-Gate-SteamVR\60Hz-direct`.
4. Внутри **xr_tracking** должны находиться `driver.vrdrivermanifest`,
   `bin\win64\driver_xr_tracking.dll`, `resources` и BAT-файлы.

CI также собирает варианты 90 Hz и `extended_sbs`. Начните с 60 Hz. Одновременно
регистрируйте только один пакет XR Gate. При публикации релиза этим workflow
четыре Windows ZIP и файлы SHA-256 также появляются в Release.

## 2. Установить SteamVR-драйвер

Установите Steam и SteamVR через Steam. Один раз запустите SteamVR, затем
закройте его. В распакованной папке **xr_tracking** дважды щёлкните
**Install-Driver.bat**. Права администратора обычно не требуются. Скрипт ищет
SteamVR по зарегистрированному runtime и установке Steam, а если не находит —
просит путь к папке SteamVR, например `D:\SteamLibrary\steamapps\common\SteamVR`.

Скрипт вызывает `vrpathreg.exe adddriver` и показывает список путей. После
регистрации оставьте папку драйвера на месте. Если SteamVR заблокировал add-on,
откройте **Settings → Startup/Shutdown → Manage Add-ons** и включите XR Gate.

Для явного пути в PowerShell:

```powershell
.\Manage-Driver.ps1 -Action Install -SteamVR 'D:\SteamLibrary\steamapps\common\SteamVR'
```

Для удаления или смены варианта закройте SteamVR, запустите
**Uninstall-Driver.bat** в старом пакете, затем установите новый. Удаляется только
этот зарегистрированный путь драйвера; SteamVR и другие драйверы сохраняются.

## 3. Подготовить SBS и видеоподключение XREAL

Это разные операции: **SBS/3D** задаёт стереорежим дисплея очков, а
**Direct Mode SteamVR** передаёт поддерживаемый дисплей под управление композитора.

1. Подключите очки к USB-C с поддержкой видео либо к подходящему адаптеру
   DisplayPort/HDMI → USB-C с питанием, подключённому к нужному GPU.
2. До передачи USB XREAL в WSL включите полный SBS/3D. У XREAL Air 2 Ultra
   удерживайте **Brightness Up (+) примерно 3 секунды** до сигнала, как указано
   в корневом README XR Gate.
3. В настройках дисплея Windows проверьте стереоразрешение очков, обычно
   **3840×1080**, и выберите **60 Hz** для пакета 60 Hz. Эта временная проверка
   обычного монитора выполняется до захвата дисплея Direct Mode.
4. Передайте камеры/HID и при необходимости Bluetooth-донгл в WSL, затем
   запустите XR Gate. Оставьте консоль `xr_client` открытой. Если WSL предлагает
   выбор режима, используйте текущий режим дисплея, когда Windows/кнопка очков
   уже переключили SBS.

После передачи USB в WSL Windows-утилита HID не может управлять тем же
устройством. Используйте кнопку очков или Linux-helper; перед Windows-helper
остановите XR Gate и верните USB Windows. Переключение режима с переподключением
USB может потребовать повторить attach и перезапустить контейнер/runtime.

## 4. Включить Direct Mode в SteamVR

1. При работающем XR Gate запустите **Test-TCP.bat**. Должен появиться
   `xr_gate_app_v1`. Успешный TCP ещё не подтверждает качество трекинга:
   проверьте startup gate и `pose_valid`.
2. Запустите SteamVR. Откройте **Settings → Developer → Enable Direct Mode**,
   как описано у Valve. В зависимости от версии включите Advanced Settings
   либо откройте Developer Settings из меню SteamVR, чтобы найти этот пункт.
3. Если требуется, перезапустите SteamVR. Проверьте изображение VR в очках и
   реакцию головы/контроллеров, затем запускайте игру.

**Direct-Mode-Guide.bat** открывает эту инструкцию и запускает SteamVR.
Переключатель Direct Mode остаётся ручным шагом: комплект не использует
недокументированную GPU-команду и не обещает, что `directMode: true` принудительно
переведёт любой монитор в Direct Mode.

Совместимость конкретного EDID XREAL, GPU и адаптера с Direct Mode нужно
проверять на реальном компьютере. Текущий драйвер предоставляет
`IVRDisplayComponent`, но не добавляет новый Windows-драйвер GPU/дисплея.
Успешная сборка DLL или TCP-тест не подтверждают возможность захвата этого
дисплея Windows в Direct Mode.

Если экран чёрный или Direct Mode недоступен, проверьте видеоподключение к GPU,
SBS-разрешение/частоту и логи SteamVR. **Экспериментальный** запасной вариант:
удалить регистрацию direct-пакета и установить соответствующий
`60Hz-extended_sbs`. Он использует обычный дисплей Windows; координаты его окна
может понадобиться изменить в `resources/settings/default.vrsettings`.
Этот путь также требует проверки с оборудованием. Для обычного использования
очков остановите SteamVR, используйте Disable Direct Mode, если пункт доступен,
верните USB из WSL и переключите очки в 2D аппаратной кнопкой.

## TCP-настройки и ежедневный запуск

Windows-пакет задаёт `tcp` для `runtimePoseTransport`, `runtimeHandTransport` и
`runtimeControllerStateTransport`. Настройки в секции `xr_tracking` файла
`resources/settings/default.vrsettings` меняйте при остановленном SteamVR:

```json
"runtimeTcpHost": "127.0.0.1",
"runtimeTcpPort": 45810,
"runtimeTcpReconnectMs": 500,
"runtimeTcpTimeoutMs": 1000
```

Host — IPv4-адрес или `localhost`. Проверки max-age головы/рук/контроллеров
сохраняются, обычно 250 мс. Одно фоновое TCP-соединение обслуживает голову и
обе руки. При разрыве кеш очищается; переставшие обновляться или повторяемые
кадры устаревают. TCP не продлевает ввод старым механизмом controller input hold.
В Linux `auto` остаётся POSIX SHM; в Windows `auto` выбирает TCP. Явный Windows
UDP остаётся доступен, но этот сценарий и WSL-комплект используют TCP.

Ежедневный порядок: **SBS/видео → USB в WSL → XR Gate → TCP-тест → SteamVR → игра**.
Docker публикует `127.0.0.1:45810` Linux-хоста; WSL передаёт localhost в Windows.
Не запускайте одновременно прямой WSL-runtime и Docker-runtime на одном порту.
Для 90 Hz используйте соответствующий пакет; частоту TCP при необходимости
можно повысить до 90 отдельно от физической частоты дисплея очков.

## Сборка из исходников и CI

Нужны MSVC x64, CMake/Ninja, Python 3.10+, OpenVR SDK commit
`0924064316de3effbcd1acf1e309182a2deb1c05` и nlohmann/json 3.11+ (в CI — 3.11.3).
В x64 Developer PowerShell из корня репозитория:

```powershell
.\drivers\openvr_driver\scripts\windows\build_driver.ps1 `
  -OpenVrSdkRoot C:\src\openvr -NlohmannJsonInclude C:\src\json\single_include `
  -Device xreal_ultra -BuildType Release -DisplayFrequency 60 -DisplayMode direct -BuildTests
ctest --test-dir build\drivers\openvr_driver\windows_Release -C Release --output-on-failure
```

Результат — `build\drivers\openvr_driver\windows_Release\xr_tracking`. Отдельный
Windows-job выполняет эти тесты и упаковывает варианты 60/90 Hz direct/extended.
В hosted CI нет оборудования: загрузку SteamVR, GPU Direct Mode, USB XREAL и
Bluetooth необходимо проверять на устройстве.

Источники: [Valve: Direct Mode](https://help.steampowered.com/en/wizard/HelpWithGameIssue/?appid=250820&issueid=370&nodeid=52),
[корневой README XR Gate](https://github.com/vladoshub/xr-gate#quick-start),
[Microsoft: сеть WSL](https://learn.microsoft.com/en-us/windows/wsl/networking).
