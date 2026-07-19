# XREAL Ultra Linux scripts

This directory now contains only XREAL-specific runtime helpers, device access
setup, and backward-compatible entrypoints. The actual Linux build/package
entrypoints are hardware-neutral and live under `devices/common`.

Preferred build:

```bash
./devices/common/linux/scripts/build/install_xr_gate_out.sh
```

Result:

```text
out/xr-gate/
```

The package contains both `xreal_ultra` and `leap_motion_uvc_nrf54l15` runtime
profiles. Select one at launch:

```bash
out/xr-gate/run_xr_client.sh --config xreal_ultra
out/xr-gate/run_xr_client.sh --config leap_motion_uvc_nrf54l15
```

Pure vendor components such as `xreal_display_helper` are enabled by default.
Disable their build and package hooks with:

```bash
XR_BUILD_VENDOR_COMPONENTS=0 \
  ./devices/common/linux/scripts/build/install_xr_gate_out.sh
```

For a local GitHub Actions build without vendor components:

```bash
./devices/common/linux/scripts/ci/run_xr_gate_act_build.sh --no-vendor-components
```

The old commands remain compatibility wrappers and now produce the same generic
package:

```bash
./devices/xreal_ultra/linux/scripts/install_xreal_ultra_out.sh
./devices/xreal_ultra/linux/scripts/package_xreal_ultra_out.sh
./devices/xreal_ultra/linux/scripts/run_xreal_ultra_act_build.sh
```
