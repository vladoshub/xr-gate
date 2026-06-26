# Monado Patches

This directory contains patches against the pinned upstream Monado checkout used by the
`drivers/monado_driver` integration.

Do not store full modified upstream Monado files here. Keep only patch files that apply
to the selected Monado commit.

## Pinned upstream

Current Monado upstream reference:

```text
https://gitlab.freedesktop.org/monado/monado.git
7363fee94b66671efdce79655b8b143d7c9eeecd
```

The install script checks out this commit in:

```text
third_party/monado_driver
```

and then applies the patches from this directory.

## Patch list

### `0001-add-xr-tracking-runtime-driver.patch`

Main Monado integration patch. It keeps all upstream Monado modifications in one
replayable patch while the project-owned driver sources remain outside the upstream
tree under `drivers/monado_driver/src`.

Patch scope:

```text
- register the project-owned `xr_tracking_runtime` driver in Monado build files
- link `drv_xr_tracking_runtime` into `monado-service`
- add the runtime driver to the legacy builder path
- enable the runtime HMD + left/right controller set when:
    XR_TRACKING_RUNTIME_ENABLE=1
- sanitize `oxr_session_locate_views()` result poses so the OpenXR `xrLocateViews`
  path does not return a zero quaternion when the runtime driver provides identity
  HMD/view poses
```

The OXR sanitize section is intentionally part of this integration patch because it
is an upstream Monado behavior workaround required by the `xr_tracking_runtime`
OpenXR path on the pinned commit. Without it, `hello_xr` may fail with:

```text
XR_ERROR_RUNTIME_FAILURE in xrLocateViews: Quaternion 0x0p+0 0x0p+0 0x0p+0 0x0p+0 ... was invalid
```

## Normal install flow

Use the driver install script from the project root:

```bash
cd ~/src/xr_tracking

drivers/monado_driver/scripts/linux/install.sh
```

For a fast rebuild without apt/submodule work:

```bash
cd ~/src/xr_tracking

BIN_DIR="$PWD/out/xreal_ultra/bin/drivers/monado_driver" \
XR_BIN_ROOT="$PWD/out/xreal_ultra/bin" \
BUILD_DIR="$PWD/third_party/monado_driver/build/xr_tracking_relwithdebinfo" \
CLONE_MONADO=0 \
FETCH_MONADO=0 \
UPDATE_SUBMODULES=0 \
INSTALL_APT_DEPS=0 \
BUILD_MONADO=1 \
drivers/monado_driver/scripts/linux/install.sh
```

The install script should reset the upstream checkout before applying project patches:

```text
RESET_MONADO_CHECKOUT=1
```

This avoids stale upstream modifications and guarantees that the current version of
`0001-add-xr-tracking-runtime-driver.patch` is applied from a clean pinned Monado
state.

## Manual patch application

Manual application is normally not needed, but for debugging the integration patch can
be checked directly:

```bash
cd ~/src/xr_tracking/third_party/monado_driver

git reset --hard 7363fee94b66671efdce79655b8b143d7c9eeecd

git apply --check ../../drivers/monado_driver/patches/monado/0001-add-xr-tracking-runtime-driver.patch
git apply ../../drivers/monado_driver/patches/monado/0001-add-xr-tracking-runtime-driver.patch
```

After applying, verify that the OXR locate-views sanitize code is present:

```bash
grep -n "oxr_sanitize_locate_views_relation" \
  src/xrt/state_trackers/oxr/oxr_session.c
```

## Runtime smoke test

Start the Monado runtime driver through the packaged XREAL Ultra script:

```bash
cd ~/src/xr_tracking/out/xreal_ultra

XR_TRACKING_RUNTIME_ENABLE=1 \
XR_TRACKING_MONADO_COMPOSITOR_MODE=xcb_fullscreen \
devices/xreal_ultra/linux/scripts/monado_driver/start_monado_driver.sh
```

Then run an OpenXR test client from another terminal:

```bash
cd ~/src/xr_tracking

XR_RUNTIME_JSON=/home/vlados/src/xr_tracking/third_party/monado_driver/build/xr_tracking_relwithdebinfo/openxr_monado-dev.json \
hello_xr -G Vulkan
```

Expected runtime selection:

```text
Head:  xr_tracking_runtime HMD
Left:  xr_tracking_runtime left controller
Right: xr_tracking_runtime right controller
```

## XCB fullscreen display note

For the NVIDIA/HDMI fallback path, `xcb_fullscreen` works best when XREAL is the only
active X11 framebuffer. Use the wrapper script that temporarily switches the display
layout and restores the main monitor on exit:

```bash
cd ~/src/xr_tracking/out/xreal_ultra

XR_TRACKING_MONADO_COMPOSITOR_MODE=xcb_fullscreen \
XR_TRACKING_MONADO_XCB_OUTPUT=DisplayPort-1-1 \
devices/xreal_ultra/linux/scripts/monado_driver/double_display_fix.sh
```

`xcb` windowed mode can be useful for diagnostics, but the best current fallback VR
experience is exclusive XREAL fullscreen with automatic monitor restore.
