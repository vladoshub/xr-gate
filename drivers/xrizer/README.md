# xrizer integration

`xrizer` is an OpenVR-to-OpenXR compatibility layer used to run OpenVR/SteamVR
applications through an OpenXR runtime without starting the SteamVR compositor.

In this project it is intended primarily for the USB4/iGPU and hybrid GPU paths
where native SteamVR direct-display acquisition is fragile, while Monado's XCB
compositor path is usable.

Recommended runtime path:

```text
OpenVR application
  -> xrizer
  -> OpenXR loader
  -> Monado OpenXR runtime
  -> XR Tracking Monado driver / XREAL display path
```

Ownership split:

- `third_party/xrizer` is the upstream clone.
- `drivers/xrizer` contains project-owned install, launch, registration, and log
  collection scripts.

## Build/install

```bash
cd ~/src/xr_tracking

devices/xreal_ultra/linux/scripts/install_xreal_ultra_out.sh
```

`xrizer` is part of the default XREAL Ultra Linux package build. Build only
this component with:

```bash
cd ~/src/xr_tracking

XR_BUILD_ONLY="xrizer" \
  devices/xreal_ultra/linux/scripts/install_xreal_ultra_out.sh
```

or directly:

```bash
cd ~/src/xr_tracking

drivers/xrizer/scripts/linux/install_xrizer.sh
```

The installed runtime directory is normally:

```text
bin/drivers/xrizer/runtime
```

For packaged output it is normally:

```text
out/xreal_ultra/bin/drivers/xrizer/runtime
```

## Register as OpenVR runtime

```bash
cd ~/src/xr_tracking/out/xreal_ultra
./run_xrizer_register.sh
```

This updates:

```text
$XDG_CONFIG_HOME/openvr/openvrpaths.vrpath
```

and places the xrizer runtime path at the front of the `runtime` array.

## Steam launch options

Print launch options for a Steam game:

```bash
cd ~/src/xr_tracking/out/xreal_ultra
./run_xrizer_openvr_app_via_monado.sh --print-steam-options
```

Typical result:

```bash
XR_RUNTIME_JSON=/path/to/openxr_monado-dev.json \
VR_OVERRIDE=/path/to/xrizer/runtime \
PRESSURE_VESSEL_IMPORT_OPENXR_1_RUNTIMES=1 \
PRESSURE_VESSEL_FILESYSTEMS_RW=$XDG_RUNTIME_DIR/monado_comp_ipc \
%command%
```

Start Monado first, then launch the game with those options.

## Logs

```bash
cd ~/src/xr_tracking/out/xreal_ultra
./run_xrizer_collect_logs.sh
```

Main xrizer log:

```text
~/.local/state/xrizer/xrizer.txt
```

## Build notes

The integration pins upstream xrizer to commit
`31319560c1bd0f1e5c16936a946bb1c7295dbfd9` by default. Override with
`XRIZER_REF=<ref>` only for explicit compatibility experiments.

When built through `devices/xreal_ultra/linux/scripts/install_xreal_ultra_out.sh`,
`XRIZER_INSTALL_RUSTUP` defaults to `1`, so the package build can install a
user-local Rust toolchain if `cargo` is missing. When calling the installer
directly, the installer does not install Rust automatically by default. If
`cargo` is missing, either install Rust manually or allow user-local rustup
installation:

```bash
XRIZER_INSTALL_RUSTUP=1 drivers/xrizer/scripts/linux/install_xrizer.sh
```

The script also ensures `rust-src`, `cargo-xbuild`, and required Ubuntu
system build dependencies when possible. The default system package set is:

```text
build-essential pkg-config clang libclang-dev glslc
```

Disable those automatic helper steps with `XRIZER_INSTALL_RUST_SRC=0`,
`XRIZER_INSTALL_CARGO_XBUILD=0`, or `XRIZER_INSTALL_SYSTEM_DEPS=0`. Override the
APT package list with `XRIZER_SYSTEM_PACKAGES=...` if a distro needs different
package names. The installer also exports `LIBCLANG_PATH` and
`BINDGEN_EXTRA_CLANG_ARGS` when possible so bindgen can find clang resource and
GCC/system headers such as `stddef.h`.
