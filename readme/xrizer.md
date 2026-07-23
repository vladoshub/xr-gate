# Xrizer

`xrizer` is optional. It is not built by default and is not part of the core binary release.

Upstream `xrizer` is GPL-3.0-or-later. If you redistribute `xrizer` binaries, keep the upstream license notices and provide the corresponding source as required by GPL-3.0-or-later.

## Build xrizer only

```bash
mkdir -p ~/src
cd ~/src

git clone https://github.com/vladoshub/xr-gate.git xr_tracking
```

```bash
cd ~/src/xr_tracking

XR_BUILD_ONLY=xrizer \
XR_BUILD_XRIZER=1 \
XR_PACKAGE_ALLOW_PARTIAL=1 \
XRIZER_PACKAGE_COMPLIANCE=1 \
XRIZER_VENDOR_CARGO_SOURCES=1 \
XRIZER_INSTALL_RUSTUP=1 \
CLONE_XRIZER=1 \
./devices/common/linux/scripts/build/install_xr_gate_out.sh
```


## Run CLI

```bash
cd ~/xr-gate-release/xr-gate
./run_xr_client.sh
```

## Run monado_driver

```bash
cd ~/xr-gate-release/xr-gate

XR_TRACKING_MONADO_COMPOSITOR_MODE=xcb \
XR_TRACKING_MONADO_XCB_OUTPUT=DisplayPort-1 \
devices/xreal_ultra/linux/scripts/monado_driver/start_monado_driver.sh
```

## Register xrizer once

```bash
cd ~/src/xr_tracking/out/xr-gate
./bin/scripts/drivers/xrizer/register_xrizer_openvrpaths.sh
```

Print Steam launch options:

```bash
cd ~/src/xr_tracking/out/xr-gate
XR_RUNTIME_JSON=~/xr-gate-release/xr-gate/bin/drivers/monado_driver/openxr_monado_xrgate.json \
./bin/scripts/drivers/xrizer/start_openvr_app_via_monado.sh --print-steam-options
```

## Steam launch options example

Take `VR_OVERRIDE` from previous terminal and put this into Steam → Game → Properties → Launch Options:

```text
XR_RUNTIME_JSON=~/xr-gate-release/xr-gate/bin/drivers/monado_driver/openxr_monado_xrgate.json VR_OVERRIDE=~/src/xr_tracking/out/xr-gate/bin/drivers/xrizer/runtime PRESSURE_VESSEL_IMPORT_OPENXR_1_RUNTIMES=1 PRESSURE_VESSEL_FILESYSTEMS_RW=/run/user/1000/monado_comp_ipc %command%
```
