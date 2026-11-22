# XR Gate в Docker на Linux

[English](README.md) · Русский · [Офлайн-инструкция](START-HERE.html?lang=ru)

## 1. Скачайте XR Gate

Подходит Ubuntu 24.04 на x86-64 с AVX2 или на ARM64. Выберите `XR_VERSION`
с архивом для Ubuntu 24.04 и вашей архитектуры.

Скачайте [ZIP](https://github.com/vladoshub/xr-gate/archive/refs/heads/main.zip), распакуйте его и откройте терминал в папке `docker`. Все команды ниже выполняйте из этой папки.

## 2. Установите Docker

Если Docker Engine версии 28 или новее и Docker Compose уже установлены, пропустите этот шаг.

```bash
sudo bash scripts/install-engine-ubuntu.sh
```

## 3. Соберите XR Gate

```bash
cp --no-clobber .env.example .env
chmod +x xr-gate
```

Чтобы выбрать релиз, измените `XR_VERSION` в `.env`. Параметры оборудования находятся в `hardware/xreal_ultra.env`. Для другого устройства скопируйте `hardware/generic.env.example`, заполните `XR_PROFILE`, `XR_USB_IDS` и пути устройств, затем укажите новый файл в `XR_HARDWARE_ENV` в `.env`. Если профиль использует камеру, включите `XR_REQUIRE_CAMERA=1`.

Для сборки из локальных архивов укажите `XR_RELEASE_SOURCE=local` в `.env`.
Положите в `docker/archives/` архив XR Gate для Ubuntu 24.04 и вашей архитектуры
(`xr-gate-ubuntu-24.04-linux-x64.tar.gz` или `xr-gate-ubuntu-24.04-linux-arm64.tar.gz`)
и `hand-tracking-models-mercury.tar.gz`. В `XR_VERSION` укажите тег этого релиза.
Если в той же папке есть `body-tracking-models-frame.tar.gz`, сборка установит и его.

Затем соберите образ:

```bash
sudo ./xr-gate build
```

Для загрузки базового образа и пакетов Ubuntu может понадобиться интернет.

## 4. Запустите XR Gate

Подключите выбранное устройство и выполните:

```bash
sudo ./xr-gate run
```

Следуйте подсказкам в консоли и оставьте её открытой. Если очки уже в режиме SBS, при запросе выберите текущий режим дисплея.

## 5. Остановите и запустите снова

Для остановки нажмите **Ctrl+C** или выполните в другом терминале:

```bash
sudo ./xr-gate stop
```

Для следующего запуска снова выполните `sudo ./xr-gate run`.

После переподключения устройства остановите XR Gate и запустите заново.

## 6. Дополнительная настройка

- [Контроллеры Gear VR и VR-PARK](docs/CONTROLLERS.ru.md).
- [Свой конфиг](config/README.md).
- Для смены релиза остановите XR Gate, измените `XR_VERSION` в `.env`, затем повторите сборку и запуск.

## 7. Если не запускается

Проверьте подключённые устройства:

```bash
sudo ./xr-gate doctor
```

Логи работающего контейнера:

```bash
sudo ./xr-gate logs
```
