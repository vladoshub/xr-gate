# Проверенные версии и источники

Проверка: **2026-10-16 UTC**.

| Компонент | Зафиксировано |
| --- | --- |
| XR Gate | `v2.3.0`, релиз от 2026-10-16 |
| Runtime archive | `xr-gate-ubuntu-24.04-linux-x64.tar.gz` |
| CPU requirement архива | `x86-64-v3` |
| Mercury models | `hand-tracking-models-mercury.tar.gz` из того же тега |
| Microsoft WSL | актуальный stable `3.0.3`, 2026-10-09; kernel в release notes `6.18.54.1` |
| Тип Ubuntu distro | WSL 2 |
| Docker Engine | локальный rootful, 28+ |

SHA-256 обоих загруженных assets проверены. Значения находятся в `release.lock.json`; загрузка при сборке образа сверяет их заново. Ubuntu base image и APT packages обновляются в пределах Ubuntu 24.04; полный побайтовый digest образа не закреплён.

Первичные источники:

- [XR Gate v2.3.0: release assets](https://github.com/vladoshub/xr-gate/releases/tag/v2.3.0).
- [XR Gate: native application TCP server](https://github.com/vladoshub/xr-gate/blob/cb25778d88f40fafea1e6b573852de523504cbb2/runtime_adapters/xr_runtime_adapter/src/app_tcp_server.cpp).
- [XR Gate: runtime launcher and TCP environment](https://github.com/vladoshub/xr-gate/blob/cb25778d88f40fafea1e6b573852de523504cbb2/runtime_adapters/xr_runtime_adapter/scripts/linux/start_xr_runtime_adapter_shm.sh).
- [XR Gate: Gear VR BLE / BlueZ transport and training](https://github.com/vladoshub/xr-gate/blob/cb25778d88f40fafea1e6b573852de523504cbb2/override_controller/README.md).
- [Microsoft WSL 3.0.3 release](https://github.com/microsoft/WSL/releases/tag/3.0.3).
- [Microsoft WSL kernel configuration, current 6.18 branch](https://github.com/microsoft/WSL2-Linux-Kernel/blob/linux-msft-wsl-6.18.y/Microsoft/config-wsl).
- [Microsoft: connect USB devices to WSL](https://learn.microsoft.com/en-us/windows/wsl/connect-usb).
- [Microsoft: WSL networking and Windows localhost access](https://learn.microsoft.com/en-us/windows/wsl/networking).
- [Microsoft: WSL versions](https://learn.microsoft.com/en-us/windows/wsl/compare-versions).
- [Microsoft: WSL Containers / wslc](https://learn.microsoft.com/en-us/windows/wsl/tutorials/wsl-containers).
- [Docker: install Engine on Ubuntu](https://docs.docker.com/engine/install/ubuntu/).
- [Docker: localhost port publishing and pre-28 caveat](https://docs.docker.com/engine/network/port-publishing/).
- [Docker Desktop: USB/IP support](https://docs.docker.com/desktop/features/usbip/).

Наличие драйверов в конфигурации ядра — подтверждение возможности загрузить соответствующие модули, а не гарантия совместимости каждого USB-чипа. Фактическую версию/модули проверяет пользовательская машина; `doctor` помогает отделить проблему host/WSL от проблемы контейнера.

`LICENSE`, `LICENSES` и third-party notices самого XR Gate остаются в извлекаемом runtime. Лицензия исходного проекта — MIT; комплект упаковки также распространяется под MIT.
