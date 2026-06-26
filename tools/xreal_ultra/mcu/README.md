# XREAL Ultra MCU tools — dangerous, opt-in only

This directory contains wrappers for building and running MCU-related tools
vendored from `TheJackiMonster/nrealAirLinuxDriver`.

**Danger:** these tools talk directly to the glasses MCU. They are not part of
the normal XR tracking runtime. We do **not** guarantee that they work with every
XREAL/NREAL model or firmware version. Using MCU tools incorrectly, especially
firmware upgrade functionality, can make the device unusable or permanently
brick it.

Default policy:

- MCU tools are never started by `xr_client`.
- MCU tools are not required for Basalt, Mercury, xr_video, xr_spatial, OpenVR or Monado.
- MCU tools are built and launched only through scripts in `tools/xreal_ultra/mcu/scripts`.
- Firmware upgrade requires an explicit confirmation environment variable.

## Vendored upstream source

The required upstream source is vendored into the repository under:

```text
third_party/nrealAirLinuxDriver
```

This avoids depending on a live external GitLab repository during normal build
or package preparation. The vendored snapshot keeps the upstream MIT license in:

```text
third_party/nrealAirLinuxDriver/LICENSE.md
```

Do not move this code into project-owned folders.

Optional refresh from a downloaded full upstream archive:

```bash
tools/xreal_ultra/mcu/scripts/prepare_nreal_upstream_from_archive.sh /path/to/nrealAirLinuxDriver.zip
```

The build script compiles only the MCU examples directly against system hidapi.
It does not require upstream hidapi/Fusion submodules because normal runtime does
not use these tools.

## Build

```bash
tools/xreal_ultra/mcu/scripts/build_mcu_tools.sh
```

The build installs binaries into `bin/tools/xreal_ultra/mcu` or, in package mode,
`$XR_BIN_ROOT/tools/xreal_ultra/mcu`.

## Run passive MCU debug monitor

```bash
tools/xreal_ultra/mcu/scripts/run_debug_mcu.sh
```

## Firmware upgrade

Firmware upgrade is intentionally blocked by default. Only run it if you fully
understand the risk:

```bash
I_UNDERSTAND_MCU_RISK=1 tools/xreal_ultra/mcu/scripts/run_mcu_firmware_upgrade.sh /path/to/firmware.bin
```
