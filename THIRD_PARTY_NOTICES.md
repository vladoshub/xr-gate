# Third-party notices

Project-owned source code is licensed under the MIT License unless a file or directory states otherwise.

Third-party source code, upstream patches, downloaded model files, generated binaries, SDKs, and optional tools remain under their respective upstream licenses. This file is a practical inventory for the default XREAL Ultra build/release path and optional components.

## Default/core components

### Basalt

- Upstream: https://github.com/VladyslavUsenko/basalt
- Use: 6DoF VIO backend integration.
- License: BSD-3-Clause upstream.
- Packaging note: keep upstream copyright/license notices with source or binary redistributions.

### Monado

- Upstream: https://gitlab.freedesktop.org/monado/monado
- Use: Monado driver/runtime integration and Mercury hand-tracking integration work.
- License: upstream per-file licensing, commonly BSL-1.0 in Monado code.
- Packaging note: Monado-derived or Monado-compatible files with explicit SPDX headers keep those licenses.

### OpenVR SDK

- Upstream: https://github.com/ValveSoftware/openvr
- Use: SteamVR/OpenVR driver integration.
- License: BSD-3-Clause upstream.
- Packaging note: do not vendor proprietary SteamVR runtime files/assets.

### nrealAirLinuxDriver

- Upstream: https://gitlab.com/TheJackiMonster/nrealAirLinuxDriver
- Use: MIT-licensed XREAL/NREAL Linux driver reference for capture/display work.
- License: MIT upstream.
- Packaging note: preserve upstream notices when adapted code is used.

### ar-drivers-rs

- Upstream: https://github.com/badicsalex/ar-drivers-rs
- Use: AR glasses display/helper reference.
- License: MIT upstream.
- Packaging note: preserve upstream notices when adapted code is used.

### OpenVR-xrealAirGlassesHMD

- Upstream: https://github.com/wheaney/OpenVR-xrealAirGlassesHMD
- Use: OpenVR/XREAL behavior and configuration reference.
- Packaging note: this project should not copy upstream code unless the copied file's license is explicitly preserved.

## Optional components

### mercury_steamvr_driver / Mercury ONNX models

- Upstream: https://github.com/moshimeow/mercury_steamvr_driver
- Use: optional source for `grayscale_detection_160x160.onnx` and `grayscale_keypoint_jan18.onnx`.
- Default status: not included in the core repository/release and not downloaded by the default build.
- Download path when explicitly requested: `out/xreal_ultra/bin/hand-tracking-models/mercury/`.
- Packaging note: model files are published only as a separate optional archive, `hand-tracking-models-mercury.tar.gz`, and remain under their upstream terms. They are not covered by the project MIT license.

### xrizer

- Upstream: https://github.com/Supreeeme/xrizer
- Use: optional OpenVR-to-OpenXR compatibility tool.
- License: GPL-3.0-or-later upstream.
- Default status: not built by default and not part of the core binary release.
- Packaging note: if redistributing xrizer binaries, provide the corresponding source and comply with GPL-3.0-or-later.
