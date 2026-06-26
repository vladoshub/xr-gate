# Xrizer

`xrizer` is optional. It is not built by default and is not part of the core binary release.

Upstream `xrizer` is GPL-3.0-or-later. If you redistribute `xrizer` binaries, keep the upstream license notices and provide the corresponding source as required by GPL-3.0-or-later.

## Build xrizer only

```bash
cd ~/src/xr_tracking

XR_BUILD_ONLY=xrizer \
./devices/xreal_ultra/linux/scripts/install_xreal_ultra_out.sh
```

Equivalent explicit form:

```bash
cd ~/src/xr_tracking

XR_BUILD_XRIZER=1 XR_BUILD_ONLY=xrizer \
./devices/xreal_ultra/linux/scripts/install_xreal_ultra_out.sh
```

## Run CLI

```bash
cd ~/src/xr_tracking/out/xreal_ultra
./run_xr_client.sh
```

## Run monado_driver

```bash
cd ~/src/xr_tracking/out/xreal_ultra

XR_TRACKING_MONADO_COMPOSITOR_MODE=xcb \
XR_TRACKING_MONADO_XCB_OUTPUT=DisplayPort-1-1 \
devices/xreal_ultra/linux/scripts/monado_driver/start_monado_driver.sh
```

## Register xrizer once

```bash
cd ~/src/xr_tracking/out/xreal_ultra
./run_xrizer_register.sh
```

Print Steam launch options:

```bash
XR_RUNTIME_JSON=/home/user/src/xr_tracking/third_party/monado_driver/build/xr_tracking_relwithdebinfo/openxr_monado-dev.json \
./run_xrizer_openvr_app_via_monado.sh --print-steam-options
```

## Steam launch options example

Take `XR_RUNTIME_JSON` from your local Monado build and put this into Steam → Game → Properties → Launch Options:

```text
XR_RUNTIME_JSON=/home/user/src/xr_tracking/third_party/monado_driver/build/xr_tracking_relwithdebinfo/openxr_monado-dev.json VR_OVERRIDE=/home/user/src/xr_tracking/out/xreal_ultra/bin/drivers/xrizer/runtime PRESSURE_VESSEL_IMPORT_OPENXR_1_RUNTIMES=1 PRESSURE_VESSEL_FILESYSTEMS_RW=/run/user/1000/monado_comp_ipc %command%
```
