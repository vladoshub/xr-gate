# Linux AVX2 runtime builds

The split workflow builds in `ubuntu:24.04` and `ubuntu:26.04` containers. The
container supplies the compiler and libraries; the Ubuntu 24.04 runner label
is not the runtime ABI target. Native code targets `x86-64-v3` (AVX2/FMA/BMI),
never the runner's `-march=native` / AVX-512 capabilities.

## Fresh build sessions

Every invocation of `install_xr_gate_out.sh` / `install_device_out.sh` starts a
new random directory under `build/.sessions/`. Native build trees, ccache,
vcpkg binary packages and downloads live there and are removed on exit,
including a failed build. Interrupted directories are never restored or reused.
A reboot or a new invocation always starts with empty caches. Source checkouts
under `third_party/` can be retained; they are not compiled dependency caches.
Do not invoke two builds concurrently in the same checkout: upstream integration
edits the Basalt and Monado source trees.

GitHub Actions builds OpenCV once **per Ubuntu ABI, workflow run and attempt**.
The SDK is an intermediate artifact, not an `actions/cache` entry. Downstream
jobs accept it only if its run/attempt, Ubuntu version, source commit and CPU
baseline match. A new workflow run or attempt cannot restore it. Use **Re-run all
jobs**, not only failed jobs, when retrying a workflow: the new attempt must
rebuild its SDK too. The `act` wrapper also gives each invocation a fresh artifact
directory and dependency identity, because act can reuse synthetic GitHub IDs.
Do not restore `build/`, `vcpkg_installed/` or compiler caches manually.

## OpenCV included in native components

The build pins OpenCV 4.10.0 to commit
`71d3237a093b60a27601c20e9ee6c3e52154e8b1` and builds a static, position-independent
AVX2 SDK. Basalt, capture service, spatial processing and Monado/Mercury consume
that SDK. Basalt's vcpkg manifest no longer builds another OpenCV, Pangolin or
GTest for the runtime-only targets. vcpkg builds the remaining dependencies in
Release only. Basalt retains both float and double estimator support and its
original optimization / Eigen initialization settings, with DWARF disabled.

The SDK includes UVC/V4L2 camera capture and JPEG/PNG image codecs. FFmpeg,
GStreamer, GUI toolkits, nonfree algorithms, IPP, OpenCL and CUDA are disabled.
The default XREAL and Leap UVC profiles use V4L2. Custom camera profiles selecting
`api: gstreamer` need a separately configured SDK; this package does not provide
that backend. Python tools continue to use the target distro's `python3-opencv`
inside the locally created thin venv; native OpenCV is linked statically and
does not replace Python's cv2 or install libraries into `/usr`.

OpenCV's Apache-2.0 license permits redistribution in the main runtime with its
license and notices. The build preserves the exact OpenCV LICENSE, COPYRIGHT,
installed third-party codec notices, source commit and build configuration in
`bin/opencv/licenses/` and `LICENSES/components/opencv/`. A separate download is
not needed for this configuration. The SDK itself (headers, static archives and
CMake metadata) is a build dependency and is not put in the runtime archive.

All packaged ELF files are stripped, including shared libraries. Packaging fails
if debug information remains or any native binary requires `libopencv_*.so`.

## Build locally

Install prerequisites first (component scripts may install additional packages):

```bash
sudo apt-get update
sudo apt-get install -y build-essential cmake ninja-build pkg-config binutils ccache nasm git python3
./devices/common/linux/scripts/build/install_xr_gate_out.sh
```

The normal script creates and removes its build session automatically. Direct
`backends/basalt_vio/scripts/linux/install_basalt.sh` and
`capture_service_cpp/scripts/linux/build_capture_service_cpp.sh` calls also create
fresh sessions. For an intentionally scoped build, for example:

```bash
XR_BUILD_ONLY='basalt_vio' ./devices/common/linux/scripts/build/install_xr_gate_out.sh
```

For development with a distro OpenCV, `XR_USE_BUNDLED_OPENCV=0` disables the
bundled SDK and static-link check. Such outputs are not portable runtime builds.

Orchestration regression checks require no compilation or downloads:

```bash
python3 devices/common/linux/scripts/build/tests/test_native_build.py -v
```

## Runtime compatibility and installation

Artifacts retain their existing names:

- `xr-gate-ubuntu-24.04-linux-x64.tar.gz`
- `xr-gate-ubuntu-26.04-linux-x64.tar.gz`

Use the matching build by default. The **24.04 static-OpenCV build is also tested
on 26.04** by a separate CI job required before publication, and its installer
accepts that combination. Legacy 24.04 packages keep the strict version guard.
The unpack helper still prefers the matching distro archive and accepts an
explicit `--main` path. Never overlay two runtime archives in the same directory.

This is not a guarantee for every Linux distro: the 24.04 toolchain still sets
the minimum glibc/libstdc++ versions. Older systems, musl/Alpine and non-x86-64
systems require separate build targets. A future older-baseline build must be
tested independently; bundling OpenCV does not lower the glibc requirement.

After unpacking, run:

```bash
./devices/common/linux/scripts/runtime/install_runtime_deps_ubuntu.sh
```

`BUILD_CPU_TARGET` records the AVX2 baseline and the installer checks CPU features
before loading native code. `BUILD_OPENCV_LINKAGE` records the bundled native
OpenCV configuration. The historical `install_runtime_deps_ubuntu24.sh` entry
point remains compatible. Device access uses the device-specific installer/hook.

Final archives omit the build-machine Python venv. The runtime installer creates
it locally with system site-packages. Same-version and cross-version CI checks
install only runtime packages, scan ELF dependencies and load the Basalt CLI.
They do not test live camera capture, tracking accuracy, graphics drivers or
headset/GPU operation. Optional xrizer and Mercury model handling is unchanged.
