# XREAL Ultra Linux scripts

This directory contains only XREAL-specific package, display and device helpers.
Hardware-neutral service/backend launchers live under:

```text
devices/common/linux/scripts/
```

Use the common launchers for capture, Basalt, Mercury, `imu_3dof`, runtime
adapter, controller input, video and spatial backends. They load the selected
device environment through `XR_DEVICE_ENV` or `XR_TARGET_DEVICE`.

The XREAL directory intentionally retains:

- `xreal_display_helper/`;
- XREAL/Monado display helpers under `monado_driver/`;
- XREAL runtime dependency, build, package and unpack scripts.

Typical package launch remains:

```bash
./run_xr_client.sh
```

Direct backend launch example:

```bash
XR_DEVICE_ENV="$PWD/devices/xreal_ultra/xreal_ultra.env" \
  ./devices/common/linux/scripts/basalt_vio/start_basalt.sh
```
