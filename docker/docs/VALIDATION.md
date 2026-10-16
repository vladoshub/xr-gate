# Проверка комплекта

Дата: **2026-10-16 UTC**. Среда подготовки: Ubuntu 24.04 x64. Docker daemon, Windows/WSL и физические XREAL/Bluetooth устройства в этой среде недоступны.

## Выполнено

| Проверка | Результат |
| --- | --- |
| Скачивание обоих настоящих v2.3.0 assets | Успешно, SHA-256 совпали с release metadata |
| Реальный `install-release.py` с этими assets | Успешно: checksum, безопасные пути tar, runtime и Mercury models на ожидаемых местах |
| Конфиги `xr_client` после извлечения | Побайтово совпали с исходным release archive |
| Настоящий релизный `runtime_adapter --help` | Есть `--app-tcp`, порт 45810 и остальные ожидаемые опции |
| Upstream TCP suite на релизном бинарнике | 5 тестов прошли: конфигурация, несколько клиентов, lost/reset, opt-in default, медленный клиент, а также fragmented video frame reader |
| Наш Python TCP probe → настоящий бинарник | Получены `hello` и frame от синтетического tracking source |
| Наш PowerShell TCP probe → настоящий бинарник | Получены `hello` и frame; PowerShell выполнялся на Linux |
| Host/entrypoint tests | 7 тестов прошли: USB selection, video0 mapping, отсутствие лишних USB, literal env, отказ при missing hardware/старом или remote Engine, сохранение stdin/config, archive traversal |
| Docker Compose CLI v2.40.3, фактический merge | Оба варианта (video off/on), localhost-only TCP ports, USB mapping, read-only config, stdin/TTY/init корректны |
| PowerShell 7.5.4 | Парсинг всех Windows-скриптов и mock-тест WSL/USB-вызовов прошли; в том числе пути с пробелами |
| Upstream xr_client через наш entrypoint, `--dry-run` | Профиль из релиза загружен, device/tracking env загружены, cascade и default deferred services соответствуют релизу |
| Shell syntax | Проверена у wrapper/entrypoint/host installer/copy script |
| Исходный checkout | Reference checkout чистый; Docker/SteamVR/CI добавлены отдельным патчем в изолированном worktree, commit/push нет |

## Что нужно проверить на целевой машине

**Полный `docker build` и реальный запуск контейнера здесь не выполнялись.** Наличие готовых ELF-бинарников и корректного Compose не заменяет такую проверку. При сборке образа автоматически выполняется upstream dependency installer/checker, CPU-target check и проверка TCP-опции адаптера. `check-image` дополнительно проверяет Python-модули, модели и бинарник.

Физические capture/HID/USB controls, VIO/hand tracking, latency, pairing/reconnect Gear VR, доступ к Windows localhost через конкретную конфигурацию WSL/firewall и интерактивная Windows-консоль требуют проверки с оборудованием. Наличие соответствующих модулей в конфигурации WSL kernel проверено по исходнику Microsoft, не на установленной пользовательской системе.

Windows-скрипты написаны синтаксисом PowerShell 5.1, но фактически исполнены в mock-режиме на PowerShell 7.5.4/Linux. Нативные `wsl.exe`, `usbipd.exe`, winget и запуск установщиков Windows в этой среде не выполнялись.

## Повторить проверки

Из каталога комплекта:

```bash
python3 -m unittest discover -s tests -v
python3 tests/verify-compose.py
pwsh -NoProfile -File tests/verify-powershell.ps1
```

Первый набор использует только Python stdlib. Второму нужен настоящий `docker compose` (daemon не требуется). Третий выполняет только parse/mock-вызовы, без изменения Windows/WSL.

На целевой машине:

```bash
sudo ./xr-gate build
sudo ./xr-gate check-image
sudo ./xr-gate doctor
sudo ./xr-gate run
# После готовности runtime, в другом терминале:
python3 scripts/tcp_probe.py
```

Из Windows после запуска runtime:

```powershell
.\windows\Test-XrGate.ps1
```

## English summary

The Docker directory was moved into the repository root, with image helpers in
`image/`. Seven unit tests and actual Compose CLI rendering pass, including
localhost-only ports, camera mapping, external Gear VR CONFIG_PATH and backups.
The image uses pinned v2.3.0 release archives; no source build is added.
No Docker daemon/image build, native Windows/WSL or physical USB/BlueZ tests were
performed here. Windows SteamVR is a separate TCP driver/CI package. See
`README.en.md`, `WINDOWS.en.md` and `CONTROLLERS.en.md` for English instructions.
