# Third-party notices

Project-owned XR Gate source code is licensed under the MIT License unless a
file or directory states otherwise. Third-party source code, adapted code,
configuration files, SDKs, generated binaries, downloaded models, and optional
tools retain their respective upstream licenses.

The repository keeps baseline license texts under `LICENSES/`. Binary packaging
also collects the license files and source metadata from the exact upstream
checkouts used for the build into `LICENSES/components/`.

## Components included in the normal runtime build

### Basalt

- Upstream: https://github.com/VladyslavUsenko/basalt
- Pinned default commit: `0f3b2b52c807f70ff4e2973ce253c73329eea7bc`.
- Use: 6DoF VIO backend, `libbasalt.so`, and Basalt-format configuration files.
- License: BSD-3-Clause.
- Baseline text: `LICENSES/Basalt-BSD-3-Clause.txt`.
- Build packaging: copies the exact upstream `LICENSE`, source metadata, and
  available vcpkg `share/*/copyright` files.
- The Basalt-derived templates and generated runtime configurations remain
  subject to the Basalt notice; XR Gate-specific parameter changes are marked
  as modifications by the project documentation.

### Monado runtime/driver

- Upstream: https://gitlab.freedesktop.org/monado/monado
- Pinned default driver commit: `7363fee94b66671efdce79655b8b143d7c9eeecd`.
- Use: `monado-service`, `libopenxr_monado.so`, and OpenXR runtime integration.
- License: per-file SPDX licensing. Monado commonly uses BSL-1.0, but the full
  upstream `LICENSES/` directory from the exact checkout is distributed.
- Baseline BSL text: `LICENSES/BSL-1.0.txt`.

### Mercury hand-tracking runtime

- Base upstream: Monado.
- Pinned default Mercury Monado commit:
  `6c9804934324ac5a3e68d64938579dbbeb4d75b3`.
- Use: `libxr_mercury_runtime.so` and Mercury dataset probe.
- License: the complete Monado per-file license set plus BSL-1.0 for the
  project Mercury overlay files carrying `SPDX-License-Identifier: BSL-1.0`.
- The release keeps this component separate from the independently built
  Monado runtime because the two builds use different pinned commits.

### ONNX Runtime

- Upstream: https://github.com/microsoft/onnxruntime
- Pinned default release: `1.18.1`.
- Use: runtime dependency for Mercury inference.
- License: MIT, with additional third-party notices.
- Baseline text: `LICENSES/ONNXRuntime-MIT.txt`.
- Build packaging requires and redistributes the exact upstream `LICENSE` and
  `ThirdPartyNotices.txt` shipped in the downloaded ONNX Runtime archive.

### OpenVR SDK

- Upstream: https://github.com/ValveSoftware/openvr
- Pinned default commit: `0924064316de3effbcd1acf1e309182a2deb1c05`.
- Use: OpenVR driver and SteamVR overlay/scene binaries; packages may include
  `libopenvr_api.so`.
- License: BSD-3-Clause.
- Baseline text: `LICENSES/OpenVR-BSD-3-Clause.txt`.
- Do not redistribute proprietary SteamVR runtime files or assets.

## Adapted project source

### nrealAirLinuxDriver

- Upstream: https://gitlab.com/TheJackiMonster/nrealAirLinuxDriver
- Pinned reference commit: `9a1f55c9838cf92627cde62f9bd69269d213d134`.
- Use: XREAL camera reorder/decode logic and optional MCU tooling.
- License: MIT.
- Text: `LICENSES/nrealAirLinuxDriver-MIT.txt`.
- Detailed source mapping is also retained in
  `capture_service_cpp/THIRD_PARTY_NOTICES.md` and
  `tools/xreal_ultra/mcu/THIRD_PARTY_NOTICES.md`.

### gearVRC

- Upstream: https://github.com/uutzinger/gearVRC
- Use: Gear VR Controller protocol facts, packet layout, scale constants, and
  initialization sequencing in `override_controller`.
- License: MIT.
- Text: `LICENSES/gearVRC-MIT.txt`.
- Detailed notice: `override_controller/providers/THIRD_PARTY_NOTICES.md`.

## Build and system dependencies

XR Gate binaries use distro-provided development packages such as CLI11,
`nlohmann/json`, Eigen, OpenCV, Ceres, hidapi, libusb, Vulkan/GL, and related
window-system libraries. Most corresponding shared objects are not copied into
the release and are installed separately by the runtime dependency installer.
Header-only and template code may nevertheless be compiled into project
binaries. On Debian/Ubuntu builds, the release packager records the exact
installed package versions and copies `/usr/share/doc/<package>/copyright` into
`LICENSES/system-packages/`. This preserves package-specific multi-license
metadata rather than assigning a single guessed license to the whole package.

## Optional components

### Mercury ONNX models / mercury_steamvr_driver

- Upstream: https://github.com/moshimeow/mercury_steamvr_driver
- Pinned default commit: `e3948ace94a9f2cbd949adf50ffcc082002337cc`.
- Use: optional `grayscale_detection_160x160.onnx` and
  `grayscale_keypoint_jan18.onnx`.
- The upstream repository supplies `LICENSES/BSL-1.0.txt`; XR Gate copies that
  exact file, source metadata, and checksums next to the models.
- XR Gate does not relicense or independently certify the provenance of the
  model weights. The models are not covered by the XR Gate MIT license; when
  publishing model assets, preserve the upstream bundle and verify that the
  selected upstream revision authorizes redistribution of those exact weights.

### xrizer

- Upstream: https://github.com/Supreeeme/xrizer
- Pinned default commit: `31319560c1bd0f1e5c16936a946bb1c7295dbfd9`.
- Use: optional OpenVR-to-OpenXR compatibility layer.
- License: GPL-3.0-or-later.
- It is not built by default. When present, the existing compliance flow requires
  the exact upstream GPL license, source metadata, checksums, and Corresponding
  Source archive under `LICENSES/` and `SOURCES/xrizer/`.

## Reference-only projects

`ar-drivers-rs` and `OpenVR-xrealAirGlassesHMD` are listed in project
documentation as references. A reference alone does not import their source
license into XR Gate. Any future copied or adapted file must retain its own
upstream notice and be added to this inventory before release.
