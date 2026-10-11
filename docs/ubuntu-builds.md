# Ubuntu runtime builds

The split workflow builds every native component separately in `ubuntu:24.04`
and `ubuntu:26.04` job containers on an Ubuntu 24.04 host runner. The container
supplies the compiler and system libraries; the host runner label is not the
runtime ABI target. Optional xrizer is built for both targets as well.

Download the runtime matching the target machine:

- `xr-gate-ubuntu-24.04-linux-x64.tar.gz`
- `xr-gate-ubuntu-26.04-linux-x64.tar.gz`

Mercury models and the standalone unpack helper are shared. The unpack helper
prefers an archive matching `/etc/os-release`, and still accepts legacy names
and explicit `--main` paths. Use separate directories for different builds;
do not overlay one runtime on another.

After unpacking, run:

```bash
./devices/common/linux/scripts/runtime/install_runtime_deps_ubuntu.sh
```

The historical `install_runtime_deps_ubuntu24.sh` entry point remains compatible
with both supported versions (including device-specific wrappers). A package's
`BUILD_UBUNTU_VERSION` is checked before installing anything. Device access still
uses the existing device-specific installer or hook.

Final archives omit the build-machine Python venv. The runtime installer creates
it locally with system site-packages. Before archiving, the merge job runs the
installer and its Python/ELF checks in a clean container of the matching distro.
This checks library resolution, not headset/GPU operation.

Both runtime archives are required before release publication. Optional xrizer
Corresponding Source is verified against each runtime and published separately
for each Ubuntu version. Local `act` execution now also requires Docker to pull
both official Ubuntu container images; its host-runner image remains 24.04.
