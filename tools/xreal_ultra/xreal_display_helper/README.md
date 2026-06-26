# XREAL Display Helper

Small helper for switching XREAL Air-family glasses display modes through the
XREAL MCU HID interface.

Used by `xr_client` prestart control before capture starts.

## Layout

```text
include/xreal_display_helper/       public/internal helper declarations
src/main.cpp                        CLI and process lifetime
src/common.cpp                      platform-neutral XREAL MCU protocol/device selection
src/platform/linux/                 Linux hidapi packet I/O and Linux hints
src/platform/windows/               Windows hidapi packet I/O and Windows hints
scripts/linux/                      Linux build/start wrappers
```

The platform split is intentionally narrow: XREAL command serialization,
mode parsing, and device PID/interface selection stay in `src/common.cpp`;
only OS-specific hidapi report framing and user-facing platform hints live
under `src/platform/<os>/`.

## Typical modes

```text
60Hz high-refresh SBS/3D
90Hz high-refresh SBS/3D
```

## Linux build

```bash
cd ~/src/xr_tracking
xreal_display_helper/scripts/linux/install_xreal_helper.sh
```

Or directly:

```bash
cmake -S xreal_display_helper -B build/xreal_display_helper/relwithdebinfo \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build/xreal_display_helper/relwithdebinfo -j"$(nproc)"
```

## Linux run

```bash
bin/xreal_display_helper/xreal_display_helper --mode 90hz --keep-running
```

Existing wrappers:

```bash
xreal_display_helper/scripts/linux/start_xreal_helper_90hz.sh
xreal_display_helper/scripts/linux/start_xreal_helper_60hz.sh
```

## Windows build

Use CMake with a Windows generator and hidapi from vcpkg/conan or explicit
`HIDAPI_INCLUDE_DIR` / `HIDAPI_LIBRARY`.

Example with vcpkg:

```powershell
cmake -S xreal_display_helper -B build/xreal_display_helper/windows_relwithdebinfo `
  -G "Visual Studio 17 2022" -A x64 `
  -DCMAKE_BUILD_TYPE=RelWithDebInfo `
  -DCMAKE_TOOLCHAIN_FILE=C:\vcpkg\scripts\buildsystems\vcpkg.cmake
cmake --build build/xreal_display_helper/windows_relwithdebinfo --config RelWithDebInfo
```

## Windows run

```powershell
.\build\xreal_display_helper\windows_relwithdebinfo\RelWithDebInfo\xreal_display_helper.exe --mode 90hz --keep-running
```

## Package output

```text
out/xreal_ultra_windows/bin/xreal_display_helper/ (Windows package) or out/xreal_ultra/bin/xreal_display_helper/ (Linux package)
```
