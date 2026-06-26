# Monado Mercury upstream patch

This directory contains patches against upstream Monado Mercury hand tracking code.

## Upstream

Repository:

```text
https://gitlab.freedesktop.org/monado/monado.git
```

Pinned commit used by the XR tracking project:

```text
7363fee94b66671efdce79655b8b143d7c9eeecd
```

## Patch

Main patch:

```text
mercury_xr_upstream_changes.patch
```

Apply it from the Monado checkout root, for example:

```bash
cd ~/src/xr_tracking/third_party/monado_driver

git checkout 7363fee94b66671efdce79655b8b143d7c9eeecd
git reset --hard HEAD

git apply --check ../../backends/mercury_hand_tracking/patches/monado/mercury_xr_upstream_changes.patch
git apply ../../backends/mercury_hand_tracking/patches/monado/mercury_xr_upstream_changes.patch
```

If the patch is stored under another backend package, keep the same rule: apply it from the root of the Monado checkout, not from the XR project root.

## What the patch changes

The patch modifies upstream Mercury files under:

```text
src/xrt/tracking/hand/mercury/
```

Touched upstream files:

```text
src/xrt/tracking/hand/mercury/CMakeLists.txt
src/xrt/tracking/hand/mercury/hg_model.cpp
src/xrt/tracking/hand/mercury/hg_sync.cpp
src/xrt/tracking/hand/mercury/hg_sync.hpp
```

High-level scope:

1. Adds Mercury build targets used by the XR tracking project:
   - `xr_mercury_runtime` shared library, a small C ABI shim for project-owned backends.
   - `xr_mercury_dataset_probe`, an offline/debug executable for replaying captured stereo datasets through Mercury.
2. Adds XR-specific detector compatibility paths:
   - detector-only horizontal mirror with ROI unmirror back to raw image coordinates;
   - optional detector slot swap;
   - optional raw + mirrored detector fusion;
   - optional stereo-paired fusion candidate selection.
3. Adds debug instrumentation for Mercury detector/fusion behavior:
   - CSV dumps;
   - ROI overlay dumps;
   - active/keypoint/optimizer overlay dumps;
   - joint number and skeleton overlays;
   - diagnostic-only overlay mirroring for slot/source analysis.

The patch intentionally keeps these changes as upstream Monado/Mercury modifications. Project-owned runtime/backend code should live outside the Monado checkout and should only depend on the exposed ABI/tools.


## Current working runtime mode

The current working hand-tracking path is the **plain Mercury path on corrected camera input**, without detector fusion and without the earlier detector compatibility experiments enabled.

Use a corrected Mercury/capture profile where the stereo images are already in the expected orientation and resolution, with matching calibration. In this mode the Mercury patch is still needed for the runtime ABI and debug/probe tools, but the runtime should normally leave the detector experiment variables unset:

```bash
# current/default runtime mode
unset MERCURY_XR_DETECTOR_MIRROR_HORIZONTAL
unset MERCURY_XR_DETECTOR_SWAP_SLOTS
unset MERCURY_XR_DETECTOR_FUSION
unset MERCURY_XR_DETECTOR_FUSION_STEREO_PAIRS
```

Equivalently, keep these options at `0` in service scripts/profiles:

```bash
MERCURY_XR_DETECTOR_MIRROR_HORIZONTAL=0
MERCURY_XR_DETECTOR_SWAP_SLOTS=0
MERCURY_XR_DETECTOR_FUSION=0
MERCURY_XR_DETECTOR_FUSION_STEREO_PAIRS=0
```

The mirror/swap/fusion code in this patch is preserved for diagnostics and historical reproduction only. It was added while trying to improve Mercury tracking on wrongly oriented/flipped camera images. After fixing the capture image orientation and using a matching calibration, the practical runtime direction is raw Mercury tracking plus project-owned post-processing/stability gating outside Mercury.

Do not enable fusion by default in production/runtime profiles unless a new dataset proves it is better for that specific camera convention.

## Important environment variables

Detector compatibility and fusion:

```bash
MERCURY_XR_DETECTOR_MIRROR_HORIZONTAL=1
MERCURY_XR_DETECTOR_SWAP_SLOTS=1
MERCURY_XR_DETECTOR_FUSION=1
MERCURY_XR_DETECTOR_FUSION_STEREO_PAIRS=1
MERCURY_XR_FUSION_MIN_CONF=0.05
MERCURY_XR_FUSION_MIN_CENTER_DIST_PX=70
```

Detector/fusion debug:

```bash
MERCURY_XR_DETECTOR_DEBUG_CSV=/tmp/mercury_detector_debug.csv
MERCURY_XR_FUSION_DEBUG_CSV=/tmp/mercury_fusion_debug.csv
MERCURY_XR_FUSION_DUMP_OVERLAY_DIR=/tmp/mercury_fusion_overlay
MERCURY_XR_FUSION_DUMP_EVERY_N=30
```

Active/keypoint overlay debug:

```bash
MERCURY_XR_DEBUG_DUMP_DIR=/tmp/mercury_debug_dump
MERCURY_XR_DEBUG_DUMP_ACTIVE_ONLY=1
MERCURY_XR_DEBUG_DRAW_SKELETON=1
MERCURY_XR_DEBUG_DRAW_JOINT_NUMBERS=1
MERCURY_XR_DEBUG_DRAW_KEYPOINTS=1
MERCURY_XR_DEBUG_DRAW_OPTIMIZER=1
MERCURY_XR_DEBUG_DRAW_PREDICTIONS=0
```

Diagnostic overlay mirroring only affects debug drawings, not Mercury runtime state:

```bash
MERCURY_XR_DEBUG_MIRROR_SLOT0=1
MERCURY_XR_DEBUG_MIRROR_SLOT1=1
MERCURY_XR_DEBUG_MIRROR_SOURCE_TAG=kp,opt
MERCURY_XR_DEBUG_MIRROR_ANCHOR=palm
MERCURY_XR_DEBUG_MIRROR_AXIS=x
```

Optional dump filters:

```bash
MERCURY_XR_DEBUG_DUMP_DET_CENTER=<det_counter>
MERCURY_XR_DEBUG_DUMP_DET_RADIUS=<radius>
MERCURY_XR_DEBUG_DUMP_TIMESTAMP_CENTER=<timestamp_ns>
MERCURY_XR_DEBUG_DUMP_TIMESTAMP_RADIUS_NS=<radius_ns>
MERCURY_XR_DEBUG_DUMP_INDEX_CENTER=<index>
MERCURY_XR_DEBUG_DUMP_INDEX_RADIUS=<radius>
MERCURY_XR_CURRENT_DUMP_INDEX=<index>
MERCURY_XR_CURRENT_LABEL=<label>
```

## Typical diagnostic runs

Detector/fusion CSV only:

```bash
cd ~/src/xr_tracking/third_party/monado_driver/build/xr_tracking_relwithdebinfo

MERCURY_MIN_DETECTION_CONFIDENCE=0.05 \
MERCURY_XR_DETECTOR_FUSION=1 \
MERCURY_XR_DETECTOR_FUSION_STEREO_PAIRS=1 \
MERCURY_XR_FUSION_DEBUG_CSV=/tmp/mercury_fusion_debug.csv \
./src/xrt/tracking/hand/mercury/xr_mercury_dataset_probe --help
```

Overlay dump example:

```bash
MERCURY_XR_DEBUG_DUMP_DIR=/tmp/mercury_debug_dump \
MERCURY_XR_DEBUG_DUMP_ACTIVE_ONLY=1 \
MERCURY_XR_DEBUG_DRAW_SKELETON=1 \
MERCURY_XR_DEBUG_DRAW_JOINT_NUMBERS=1 \
./src/xrt/tracking/hand/mercury/xr_mercury_dataset_probe <dataset/options>
```

Exact dataset/probe arguments depend on the current project-owned probe wrapper and dataset layout.

## Build check

After applying the patch and copying any project-owned Mercury shim/probe sources expected by `CMakeLists.txt`, configure/build Monado as usual:

```bash
cd ~/src/xr_tracking

BUILD_MONADO=1 \
CLONE_MONADO=0 \
FETCH_MONADO=0 \
UPDATE_SUBMODULES=0 \
INSTALL_APT_DEPS=0 \
drivers/monado_driver/scripts/linux/install.sh
```

Verify the targets are present in the build tree:

```bash
find third_party/monado_driver/build -type f -name 'libxr_mercury_runtime.so' -o -name 'xr_mercury_dataset_probe'
```

## Ownership rule

Do not store full edited Monado/Mercury upstream files as project-owned source.

Use this split:

```text
project-owned files:
  stored normally in the XR tracking repository

upstream Monado/Mercury modifications:
  stored as patch files under this directory
```

When updating this integration, refresh `mercury_xr_upstream_changes.patch` against the pinned Monado commit and keep this README in sync with any new environment variables or build targets.
