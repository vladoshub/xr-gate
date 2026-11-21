# Linux CPU-profile runtime builds

The split workflow builds in `ubuntu:24.04` and `ubuntu:26.04` containers. The
container supplies the compiler and libraries; the Ubuntu 24.04 runner label
is not the runtime ABI target. Select the CPU profile with `XR_BUILD_CPU`:

| Profile | GCC/Clang target | OpenCV baseline | Intended use |
| --- | --- | --- | --- |
| `generic` | `-march=x86-64 -mtune=generic` | SSE2 | Portable x86-64 runtime without AVX/AVX2/FMA requirements |
| `avx2` (default) | `-march=x86-64-v3 -mtune=generic` | AVX2 | Shared AVX2/FMA/BMI runtime |
| `avx512` | `-march=x86-64-v4 -mtune=generic` | AVX512_SKX | CPUs with AVX-512F/CD/BW/DQ/VL |
| `native` | `-march=native -mtune=native` | NATIVE | The x86-64 build machine's CPU |
| `aarch64` | `-march=armv8-a -mtune=generic` | NEON | Portable Linux ARM64 runtime |
| `riscv64` | `-march=rv64gc -mtune=rocket` | Detect from compiler target | Linux RISC-V 64-bit runtime for RV64GC |

The same profile configures Basalt, OpenCV and the vcpkg dependencies. `generic`
uses the original x86-64 baseline (including SSE2); it does not require SSE3,
SSE4, AVX, AVX2 or FMA. It can be built on a newer CPU or a hosted GitHub runner
for use on an older x86-64 CPU. OpenCV CPU dispatch remains disabled. Unlike
`native`, this profile does not inherit the build host's instruction set.
The Ubuntu/glibc requirements below still apply, and tracking performance on
older CPUs must be checked with the actual hardware.

`avx512`
is the x86-64-v4 baseline, not every vendor-specific AVX-512 extension. `native`
can use additional extensions on the build host and is not a portable release.
For native SDK reuse, CPU model/features must match across CI jobs. On heterogeneous
hosted runners the build may reject a native SDK; use a consistent self-hosted
runner or a local build. Native runtime checks conservatively require the build
host's CPU flags, and may reject another kernel/VM even with a similar CPU.

Local examples (from the repository root):

```bash
XR_BUILD_CPU=generic ./devices/common/linux/scripts/build/install_xr_gate_out.sh
XR_BUILD_CPU=avx2 ./devices/common/linux/scripts/build/install_xr_gate_out.sh
XR_BUILD_CPU=avx512 ./devices/common/linux/scripts/build/install_xr_gate_out.sh
XR_BUILD_CPU=native ./devices/common/linux/scripts/build/install_xr_gate_out.sh
XR_BUILD_CPU=aarch64 ./devices/common/linux/scripts/build/install_xr_gate_out.sh
```

Build the `aarch64` profile on a Linux ARM64 host. It applies to every device
profile and can be used with `XR_BUILD_ONLY`.

Build the `riscv64` profile on a Linux RISC-V host with an RV64GC/LP64D
toolchain. It includes Monado and defaults to scalar MLAS in ONNX Runtime
for Mercury and FRAME. The build skips the SteamVR and xrizer components.

```bash
XR_BUILD_CPU=riscv64 ./devices/common/linux/scripts/build/install_build_deps_ubuntu.sh
XR_BUILD_CPU=riscv64 XR_DOWNLOAD_HAND_TRACKING_MODELS=1 \
  ./devices/common/linux/scripts/build/install_xr_gate_out.sh
```

Use the pre-exported FRAME model archive from the release or from an AMD64
or ARM64 model build. Extract it into the package's `bin` directory.

```bash
tar -xzf body-tracking-models-frame.tar.gz -C out/riscv64/xr-gate/bin
```

In GitHub Actions, check `build_ubuntu_amd64`, `build_ubuntu_arm64`, or
`build_ubuntu_riscv64` independently. AMD64 and ARM64 each build Ubuntu 24.04
and 26.04. AMD64 is selected by default and uses `cpu_profile`. Choose
`generic` for AMD64 CPUs without AVX2. ARM64 uses `aarch64` on
`ubuntu-24.04-arm`. RISC-V builds Ubuntu 24.04 for RV64GC in parallel
component jobs using Docker and QEMU on an AMD64 runner.
With `publish_release` enabled, the selected builds go into one GitHub Release
under `release_tag`. The Windows installer is selected independently with
`release_windows_driver`.

For a local act run, select one architecture per invocation.

```bash
./devices/common/linux/scripts/ci/run_xr_gate_act_build.sh --full --allow-pull \
  --input build_ubuntu_amd64=true --input build_ubuntu_arm64=false \
  --input release_windows_driver=false --input cpu_profile=generic --input publish_release=false
```

Local output roots are separated automatically.
`out/generic/xr-gate/` for generic, `out/xr-gate/` for AVX2,
`out/avx512/xr-gate/` for AVX512, `out/native/xr-gate/` for native,
`out/aarch64/xr-gate/` for ARM64, and `out/riscv64/xr-gate/` for RISC-V.
An explicit `XR_OUT_ROOT` still works. The
installer rejects reuse across CPU profiles and native CPU identities.
CI explicitly selects its original `out/xr-gate/` layout in each isolated job.
`BUILD_CPU_TARGET` and `BUILD_CPU_PROFILE.json` inside each package record
the selected profile. The installer checks architecture and CPU features.
The builder must also support the chosen instructions for build-time helper tools.

## ONNX Runtime version

Mercury and FRAME build with ONNX Runtime **1.31.0** on AMD64, ARM64 and
RISC-V. Set `XR_ONNXRUNTIME_VERSION=1.18.1` to select the legacy SDK for a
local build, or choose `onnxruntime_version` when dispatching the GitHub
workflow. The FRAME model check uses the selected version too.

To switch versions in an existing package, rebuild both inference backends
with the package's CPU profile and output directory. For example, to roll
back a generic AMD64 package:

```bash
XR_BUILD_CPU=generic XR_ONNXRUNTIME_VERSION=1.18.1 \
  XR_BUILD_ONLY="mercury_hand_tracking frame_body_tracking" \
  XR_DOWNLOAD_BODY_TRACKING_MODELS=0 \
  ./devices/common/linux/scripts/build/install_xr_gate_out.sh
```

Each backend loads the version it was built against. The environment variable
is only needed for the build. Use 1.31.0 for RISC-V inference, since 1.18.1
can produce incorrect scalar matrix multiplication results there.

## Cross-compilation from AMD64 Linux

### Install GNU cross-compilers

On Ubuntu 24.04 AMD64, install the toolchain for the target you need.
The cross-glibc packages supply the target's C library and headers.

```bash
sudo apt update
sudo apt install cmake ninja-build pkg-config qemu-user

# ARM64
sudo apt install gcc-aarch64-linux-gnu g++-aarch64-linux-gnu \
  binutils-aarch64-linux-gnu libc6-dev-arm64-cross

# RISC-V 64-bit
sudo apt install gcc-riscv64-linux-gnu g++-riscv64-linux-gnu \
  binutils-riscv64-linux-gnu libc6-dev-riscv64-cross
```

Verify the compiler, ELF architecture and target loader:

```bash
work="$(mktemp -d)"
printf '#include <cstdio>\nint main() { std::puts("Cross-build OK"); }\n' > "$work/test.cpp"

aarch64-linux-gnu-g++ -march=armv8-a "$work/test.cpp" -o "$work/test-arm64"
aarch64-linux-gnu-readelf -h "$work/test-arm64"
qemu-aarch64 -L /usr/aarch64-linux-gnu "$work/test-arm64"

riscv64-linux-gnu-g++ -march=rv64gc -mabi=lp64d "$work/test.cpp" -o "$work/test-riscv64"
riscv64-linux-gnu-readelf -h "$work/test-riscv64"
qemu-riscv64 -cpu rv64,v=false -L /usr/riscv64-linux-gnu "$work/test-riscv64"
```

To cross-compile an individual CMake component, pass
`-DCMAKE_TOOLCHAIN_FILE=/path/to/toolchain.cmake`. Set
`CMAKE_SYSTEM_NAME=Linux`, the target processor (`aarch64` or `riscv64`),
and the matching cross GCC/G++ in that file. Provide a sysroot or CMake
search paths containing the target builds of the component's dependencies
such as OpenCV. Use a separate CMake build directory for each target.
See the [CMake toolchain guide](https://cmake.org/cmake/help/latest/manual/cmake-toolchains.7.html)
and [ONNX Runtime cross-build guide](https://onnxruntime.ai/docs/build/inferencing.html#risc-v).

### Build the complete package using QEMU

The full package entrypoint expects a Linux environment for the target
architecture. On AMD64, run it inside an Ubuntu container with QEMU
user-mode emulation. From the repository root on the host:

```bash
sudo apt install docker.io qemu-user-static
sudo systemctl restart systemd-binfmt
```

Choose the container for your target.

| `XR_BUILD_CPU` | Docker platform | Image |
| --- | --- | --- |
| `aarch64` | `linux/arm64` | `ubuntu:24.04` |
| `riscv64` | `linux/riscv64` | `riscv64/ubuntu:24.04` |

```bash
profile=aarch64
platform=linux/arm64
image=ubuntu:24.04

sudo docker run --rm --platform "$platform" "$image" uname -m

sudo docker run --rm --platform "$platform" \
  --mount "type=bind,source=$PWD,target=/src" --workdir /src \
  -e XR_BUILD_CPU="$profile" -e XR_DOWNLOAD_HAND_TRACKING_MODELS=1 -e XR_OFFLINE=0 \
  -e XR_ONNXRUNTIME_VERSION="${XR_ONNXRUNTIME_VERSION:-1.31.0}" \
  -e XR_BUILD_UID="$(id -u)" -e XR_BUILD_GID="$(id -g)" \
  "$image" bash -eu -c '
    ./devices/common/linux/scripts/build/install_build_deps_ubuntu.sh
    export HOME=/tmp/xr-gate-builder
    install -d -o "$XR_BUILD_UID" -g "$XR_BUILD_GID" "$HOME"
    setpriv --reuid="$XR_BUILD_UID" --regid="$XR_BUILD_GID" --clear-groups \
      ./devices/common/linux/scripts/build/install_xr_gate_out.sh
  '
```

The first container should print your selected architecture. For Docker
hosts that return `exec format error`, see the
[QEMU registration instructions](https://docs.docker.com/build/building/multi-platform/#install-qemu-manually).
The build writes to Git checkouts under `third_party/`, so use a separate
source checkout per architecture if running builds concurrently. The package
is written to `out/$profile/xr-gate/` on the host.

## Local dependency cache and offline rebuilds

Run the normal build once with internet access after installing system packages.
Existing checkouts are reused; remote refs are refreshed only when explicitly
requested. After a **successful** build, the same entrypoint, component selection,
CPU profile and dependency settings automatically use offline mode. A failed or
interrupted first build does not mark the cache ready; the next run resumes
preparation of missing dependencies without refreshing existing ones.

```bash
# First successful run prepares dependencies; later identical runs are offline.
XR_DOWNLOAD_HAND_TRACKING_MODELS=1 ./devices/common/linux/scripts/build/install_xr_gate_out.sh

# Require offline operation immediately (missing inputs cause a local error).
XR_OFFLINE=1 XR_DOWNLOAD_HAND_TRACKING_MODELS=1 ./devices/common/linux/scripts/build/install_xr_gate_out.sh

# Allow filling missing dependencies without updating existing checkouts.
XR_OFFLINE=0 XR_DOWNLOAD_HAND_TRACKING_MODELS=1 ./devices/common/linux/scripts/build/install_xr_gate_out.sh

# Optional refresh step: fetch dependency metadata and run the selected build.
XR_DOWNLOAD_HAND_TRACKING_MODELS=1 ./devices/common/linux/scripts/build/update_build_dependencies.sh
# Equivalent: XR_UPDATE_DEPS=1 .../install_xr_gate_out.sh
```

Use the same component/model/xrizer flags for all commands. The refresh command
**also builds** the selected components; it does not install apt packages. It
keeps the project's pinned commits and Cargo.lock, rather than switching to the
latest upstream versions. Choose a different supported ref explicitly to change
a source version. Per-component `FETCH_BASALT`, `FETCH_MONADO`, and
`FETCH_MERCURY_DRIVER` flags remain available, but require `XR_OFFLINE=0` after
the cache has been marked ready. Prefer `XR_UPDATE_DEPS=1` for one whole build.

Source trees and initialized submodules remain in `third_party/`. Downloaded
archives, vcpkg downloads/binaries/installed dependencies,
XDG tool caches and ccache persist in `build/.dependency-cache/` by default.
`XR_DEPENDENCY_CACHE_ROOT=/absolute/path` can relocate this cache. Rust uses the
user's persistent `CARGO_HOME` and `RUSTUP_HOME` (normally `~/.cargo` and
`~/.rustup`); xrizer's first successful build must include its optional source
compliance/vendor step to prepare those dependencies too. Optional models retain
LFS objects in their checkout and hydrate locally before considering a download.
Mercury and FRAME share an ONNX Runtime SDK. Both versions build pinned
commits from `ROCm/onnxruntime`. The default uses `third_party/onnxruntime-1.31.0`
and the legacy version uses `third_party/onnxruntime`. The SDK cache is scoped
to the source commit, distro and build flags. It can restore a removed output
directory offline.

Offline builds disable Git transports, enable Cargo offline mode, block vcpkg's
origin fallback, and disable automatic Rust toolchain installation. A Linux
libseccomp filter also rejects IPv4/IPv6 sockets in the build and its children,
including upstream CMake/download commands. It needs no sudo; `libseccomp2` is
included in the system preparer. Missing inputs fail with a message suggesting
`XR_OFFLINE=0`. Keep `third_party/`, the dependency cache, and Rust caches for
future offline runs. Removing `build/` removes the default dependency cache too.

Changing the selected components, CPU profile, dependency ref/path overrides or
integration scripts gives a new first online build. You can always request
`XR_OFFLINE=1` explicitly if the necessary dependencies are already cached. The
automatic marker is local to this checkout/configuration, not an exported SDK.
Do not invoke two builds concurrently in the same checkout: upstream integration
edits the OpenCV, Basalt and Monado source trees and their dependency installs.

## Build sessions and retained OpenCV products

Every invocation starts a new random directory under `build/.sessions/` for
native build trees. OpenCV's `opencv/build/` and `opencv/install/` remain there
on exit by default, including after a failed build (`XR_KEEP_OPENCV_BUILD=1`).
The script logs their path. Other session files are removed; persistent caches
are outside the session and survive either OpenCV retention setting.

```bash
XR_KEEP_OPENCV_BUILD=0 ./devices/common/linux/scripts/build/install_xr_gate_out.sh
```

OpenCV sources are checked out in `third_party/opencv/` and reused at the pinned
commit, with an idempotent CMake compatibility patch. Conflicting developer edits
stop checkout. Retaining products does not enable incremental SDK builds: each
invocation still creates a new native build/SDK path, using the persistent local
compiler cache. Old session build trees are never restored.

The split GitHub Actions workflow (also used by `act`) retains fresh, isolated
caches per job/session and sets `XR_KEEP_OPENCV_BUILD=0`. Its automatic mode
remains online because each job starts from a clean dependency environment.

GitHub Actions builds OpenCV once **per Ubuntu ABI, workflow run and attempt**.
The SDK is an intermediate artifact, not an `actions/cache` entry. Downstream
jobs accept it only if its run/attempt, Ubuntu version, source commit and CPU
profile/native CPU fingerprint match. A new workflow run or attempt cannot
restore it. Use **Re-run all jobs**, not only failed jobs, when retrying a workflow: the new attempt must
rebuild its SDK too. The `act` wrapper also gives each invocation a fresh artifact
directory and dependency identity, because act can reuse synthetic GitHub IDs.
Do not restore local caches into these CI jobs; their SDK run/attempt checks remain strict.

## OpenCV included in native components

The build pins OpenCV 4.10.0 to commit
`71d3237a093b60a27601c20e9ee6c3e52154e8b1` and builds a static, position-independent
SDK for the selected CPU profile. Basalt, capture service, spatial processing
and Monado/Mercury consume that SDK. Basalt's vcpkg manifest no longer builds another OpenCV, Pangolin or
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

Prepare system dependencies before starting compilation. The preparer uses sudo
once for apt update/install; the package build does not invoke sudo or install
system packages. Missing packages are reported before OpenCV compilation starts.

```bash
./devices/common/linux/scripts/build/install_build_deps_ubuntu.sh
./devices/common/linux/scripts/build/install_xr_gate_out.sh
```

Preparation honors the same target selection as the build. For optional xrizer
or model downloads, use the same settings on both commands, for example:

```bash
XR_BUILD_XRIZER=1 XR_DOWNLOAD_HAND_TRACKING_MODELS=1 ./devices/common/linux/scripts/build/install_build_deps_ubuntu.sh
XR_BUILD_XRIZER=1 XR_DOWNLOAD_HAND_TRACKING_MODELS=1 ./devices/common/linux/scripts/build/install_xr_gate_out.sh
```

Use `--list` to inspect the package list or `--check` for a read-only check.
Repeat preparation when enabling additional components or after dependencies
change. All component build/download scripts, including standalone invocations,
leave system packages to the preparer. The old automatic apt/LFS installation
options are no longer used. The package builder also disables `ADD_INPUT_GROUP`
for its child processes.
Device access/udev/group changes remain in the separate runtime/device setup
scripts; they are not needed for compilation.

The normal script manages its build session and OpenCV retention automatically. Direct
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
python3 -m unittest discover -s devices/common/linux/scripts/build/tests -v
```

## Runtime compatibility and installation

The runtime archive suffix identifies the architecture.

- `xr-gate-ubuntu-24.04-linux-x64.tar.gz`
- `xr-gate-ubuntu-26.04-linux-x64.tar.gz`
- `xr-gate-ubuntu-24.04-linux-arm64.tar.gz`
- `xr-gate-ubuntu-26.04-linux-arm64.tar.gz`

Use the matching build by default. The **24.04 static-OpenCV build is also tested
on 26.04** by a separate CI job required before publication, and its installer
accepts that combination. Legacy 24.04 packages keep the strict version guard.
The unpack helper prefers the matching distro and architecture and accepts an
explicit `--main` path. Never overlay two runtime archives in the same directory.

This is not a guarantee for every Linux distro: the 24.04 toolchain still sets
the minimum glibc/libstdc++ versions. Older systems, musl/Alpine and other CPU
architectures require separate build targets. A future older-baseline build must
be tested independently. Bundling OpenCV does not lower the glibc requirement.

After unpacking, run:

```bash
./devices/common/linux/scripts/runtime/install_runtime_deps_ubuntu.sh
```

`BUILD_CPU_TARGET` and `BUILD_CPU_PROFILE.json` record the selected target; the installer checks CPU features
before loading native code. `BUILD_OPENCV_LINKAGE` records the bundled native
OpenCV configuration. The historical `install_runtime_deps_ubuntu24.sh` entry
point remains compatible. Device access uses the device-specific installer/hook.

Final archives omit the build-machine Python venv. The runtime installer creates
it locally with system site-packages. Same-version and cross-version CI checks
install only runtime packages, scan ELF dependencies and load the Basalt CLI.
They do not test live camera capture, tracking accuracy, graphics drivers or
headset/GPU operation. Optional xrizer and Mercury model handling is unchanged.
For `generic`, the Ubuntu 24.04 merge job additionally starts the capture probe
and Basalt backend under QEMU's Conroe CPU model (no AVX/AVX2/FMA). This checks
startup and linked libraries; it does not exercise a complete tracking session.
