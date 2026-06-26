# Basalt VIO integration patch

This directory contains the upstream Basalt integration patch used by the XR Tracking Basalt backend.

The patch is intentionally kept here instead of storing modified upstream Basalt files in the project tree. Project-owned backend code lives under `backends/basalt_vio`; upstream Basalt edits are represented as patch files under `backends/basalt_vio/patches/basalt`.

## Upstream baseline

Expected upstream repository:

```text
https://github.com/VladyslavUsenko/basalt
```

Expected revision:

```text
0f3b2b52c807f70ff4e2973ce253c73329eea7bc
```

## Patch file

```text
basalt_vio_integration.patch
```

This patch currently does two upstream-side things:

1. Adds an optional CMake integration hook to Basalt's top-level `CMakeLists.txt`:

   ```cmake
   set(XR_TRACKING_ROOT "$ENV{XR_TRACKING_ROOT}" CACHE PATH "Path to xr_tracking checkout")
   if(XR_TRACKING_ROOT AND EXISTS "${XR_TRACKING_ROOT}/backends/basalt_vio/CMakeLists.txt")
     add_subdirectory(
       "${XR_TRACKING_ROOT}/backends/basalt_vio"
       "${CMAKE_BINARY_DIR}/xr_tracking/backends/basalt_vio"
     )
   endif()
   ```

   This lets a Basalt build include the project-owned `backends/basalt_vio` CMake target without copying that backend into the upstream Basalt repository.

2. Removes the `realsense2` dependency block from Basalt's `vcpkg.json`.

   The XR Tracking Basalt backend consumes stereo/IMU streams from the project capture pipeline, so RealSense support is not required for this backend build.

## Apply manually

From a Basalt checkout inside this repository:

```bash
cd ~/src/xr_tracking/third_party/basalt

git checkout 0f3b2b52c807f70ff4e2973ce253c73329eea7bc

git apply --check ../../backends/basalt_vio/patches/basalt/basalt_vio_integration.patch
git apply ../../backends/basalt_vio/patches/basalt/basalt_vio_integration.patch
```

If Basalt is checked out somewhere else, use an absolute patch path:

```bash
cd /path/to/basalt

git checkout 0f3b2b52c807f70ff4e2973ce253c73329eea7bc

git apply --check /home/vlados/src/xr_tracking/backends/basalt_vio/patches/basalt/basalt_vio_integration.patch
git apply /home/vlados/src/xr_tracking/backends/basalt_vio/patches/basalt/basalt_vio_integration.patch
```

## Configure/build manually

The important part is to pass the XR Tracking checkout path to Basalt's CMake configure step:

```bash
cd ~/src/xr_tracking/third_party/basalt

export XR_TRACKING_ROOT="$HOME/src/xr_tracking"

cmake -S . -B build/xr_tracking_relwithdebinfo \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DXR_TRACKING_ROOT="$XR_TRACKING_ROOT"

cmake --build build/xr_tracking_relwithdebinfo --target capture_basalt_backend -j"$(nproc)"
```

A successful configure should print a message similar to:

```text
Enabling XR Basalt VIO backend from /home/vlados/src/xr_tracking/backends/basalt_vio
```

## Expected runtime role

The Basalt backend is responsible for VIO/HMD pose production. It should consume stereo camera and IMU streams from the XR Tracking capture pipeline and publish an HMD pose stream for `xr_runtime_adapter`.

Typical flow:

```text
capture_service_cpp
  -> capture_client stereo + IMU
  -> capture_basalt_backend
  -> hmd_pose stream
  -> xr_runtime_adapter
  -> runtime_hmd_pose / OpenVR / Monado driver consumers
```

## Ownership rule

Keep this split intact:

```text
Project-owned code:
  backends/basalt_vio/**
  shared/**
  runtime_adapters/**
  devices/**

Upstream Basalt edits:
  backends/basalt_vio/patches/basalt/*.patch
```

Do not copy full modified upstream Basalt files into the project tree unless they are deliberately vendored as third-party source. Store upstream changes as patches.

## Troubleshooting

If CMake prints:

```text
XR Basalt VIO backend disabled; set XR_TRACKING_ROOT to the xr_tracking checkout
```

then `XR_TRACKING_ROOT` is missing or points to the wrong directory. Reconfigure with:

```bash
export XR_TRACKING_ROOT="$HOME/src/xr_tracking"
cmake -S . -B build/xr_tracking_relwithdebinfo -DXR_TRACKING_ROOT="$XR_TRACKING_ROOT"
```

If `git apply --check` fails, verify that the Basalt checkout is at the expected revision and does not already contain the patch:

```bash
git rev-parse HEAD
git diff -- CMakeLists.txt vcpkg.json
grep -n "XR_TRACKING_ROOT" CMakeLists.txt
grep -n "realsense2" vcpkg.json
```

If `capture_basalt_backend` target is missing after configure, verify that the project-owned backend CMake file exists:

```bash
test -f "$XR_TRACKING_ROOT/backends/basalt_vio/CMakeLists.txt" && echo OK
```
