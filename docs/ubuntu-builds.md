# Linux CPU-profile runtime builds

The split workflow builds in `ubuntu:24.04` and `ubuntu:26.04` containers. The
container supplies the compiler and libraries; the Ubuntu 24.04 runner label
is not the runtime ABI target. Select the CPU profile with `XR_BUILD_CPU`:

| Profile | GCC/Clang target | OpenCV baseline | Intended use |
| --- | --- | --- | --- |
| `generic` | `-march=x86-64 -mtune=generic` | SSE2 | Portable x86-64 runtime without AVX/AVX2/FMA requirements |
| `avx2` (default) | `-march=x86-64-v3 -mtune=generic` | AVX2 | Shared AVX2/FMA/BMI runtime |
| `avx512` | `-march=x86-64-v4 -mtune=generic` | AVX512_SKX | CPUs with AVX-512F/CD/BW/DQ/VL |
| `native` | `-march=native -mtune=native` | NATIVE | The build machine's CPU |

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
```

For GitHub Actions, select `cpu_profile=generic` in the dispatch inputs for CPUs
without AVX2. AVX2 remains the default. With local act:

```bash
./devices/common/linux/scripts/ci/run_xr_gate_act_build.sh --full --allow-pull --input cpu_profile=generic --input publish_release=false
```

Build one profile per invocation. Local output roots are separated automatically:
`out/generic/xr-gate/` for generic, `out/xr-gate/` for AVX2,
`out/avx512/xr-gate/` for AVX512, and
`out/native/xr-gate/` for native. An explicit `XR_OUT_ROOT` still works; the
installer rejects reusing it for a different CPU profile/native CPU identity.
CI explicitly selects its original `out/xr-gate/` layout in each isolated job.
Artifact filenames are unchanged; `BUILD_CPU_TARGET` and `BUILD_CPU_PROFILE.json`
inside each package record the actual profile. The installer checks CPU support.
The builder must also support the chosen instructions for build-time helper tools.

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
archives (including ONNX Runtime), vcpkg downloads/binaries/installed dependencies,
XDG tool caches and ccache persist in `build/.dependency-cache/` by default.
`XR_DEPENDENCY_CACHE_ROOT=/absolute/path` can relocate this cache. Rust uses the
user's persistent `CARGO_HOME` and `RUSTUP_HOME` (normally `~/.cargo` and
`~/.rustup`); xrizer's first successful build must include its optional source
compliance/vendor step to prepare those dependencies too. Optional models retain
LFS objects in their checkout and hydrate locally before considering a download.
Cached ONNX archives can restore a removed output directory without downloading.

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
