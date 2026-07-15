# XREAL Ultra Linux scripts

This directory contains only XREAL-specific display/device helpers and thin
entrypoints that configure the generic tooling under `devices/common`.

Hardware-neutral service/backend launchers live under:

```text
devices/common/linux/scripts/
```

Shared build/package implementations live under:

```text
devices/common/linux/scripts/build/
devices/common/linux/scripts/ci/
devices/common/linux/scripts/release/
devices/common/linux/scripts/runtime/
```

The XREAL directory retains:

- `xreal_display_helper/`;
- XREAL/Monado display helpers under `monado_driver/`;
- XREAL naming/profile wrappers (`install_xreal_ultra_out.sh`,
  `package_xreal_ultra_out.sh`, `run_xreal_ultra_act_build.sh`,
  `unpack_xreal_ultra.sh`, `xreal_ultra_out_env.sh`);
- XREAL-only build/package hooks for `xreal_display_helper` and desktop restore;
- XREAL user-group and udev setup.

The public XREAL commands remain unchanged, for example:

```bash
./devices/xreal_ultra/linux/scripts/install_xreal_ultra_out.sh
./devices/xreal_ultra/linux/scripts/package_xreal_ultra_out.sh
```

Typical packaged runtime launch remains:

```bash
./run_xr_client.sh
```
