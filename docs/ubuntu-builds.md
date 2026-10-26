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
profile/native CPU fingerprint match. A new workflow run or attempt cannot
restore it. Use **Re-run all jobs**, not only failed jobs, when retrying a workflow: the new attempt must
rebuild its SDK too. The `act` wrapper also gives each invocation a fresh artifact
directory and dependency identity, because act can reuse synthetic GitHub IDs.
Do not restore `build/`, `vcpkg_installed/` or compiler caches manually.

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
