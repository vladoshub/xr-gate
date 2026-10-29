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
drivers/monado_driver/scripts/linux/install.sh
```

Verify the targets are present in the build tree:

```bash
find third_party/monado_driver/build -type f -name 'libxr_mercury_runtime.so' -o -name 'xr_mercury_dataset_probe'
```

## Tracking loss diagnostics

`mercury_tracking_diagnostics.patch` applies **after**
`mercury_xr_upstream_changes.patch`. The normal Mercury installer applies both
and copies `xr_mercury_tracking_diagnostics.hpp` from the project overlay.
The tracking diagnostics are disabled unless `MERCURY_XR_TRACKING_DEBUG_CSV`
is set. They do not change detector thresholds, camera selection, ROI prediction,
overlap rejection, or optimizer decisions.

For an existing configured Mercury build, rebuild only the shared library:

```bash
cd /path/to/xr-gate
bash backends/mercury_hand_tracking/scripts/linux/rebuild_tracking_diagnostics.sh
```

Set `MERCURY_BUILD_DIR` when the build lives outside the usual project paths.
The helper reads `CMAKE_HOME_DIRECTORY` from that build's cache and preserves
local Monado changes. To install into a separate release directory, set
`INSTALL_BIN_DIR=/path/to/release/bin/backends/mercury_hand_tracking`.
The old library is retained as `libxr_mercury_runtime.so.before_tracking_diagnostics`.

Enable tracing in the shell that launches the existing XR Gate setup:

```bash
MERCURY_DIAG_DIR="$HOME/xr-mercury-loss-$(date +%Y%m%d-%H%M%S)"
mkdir -p "$MERCURY_DIAG_DIR"
export MERCURY_XR_TRACKING_DEBUG_CSV="$MERCURY_DIAG_DIR/tracking.csv"
export MERCURY_XR_DETECTOR_DEBUG_CSV="$MERCURY_DIAG_DIR/detector.csv"
```

Start XR Gate with the same device/profile and settings as the failing test.
The newly rebuilt library prints `[mercury_tracking_diag] enabled schema=1`.
`tracking.csv` contains two records per input frame: hand 0 is left and hand 1
is right. Timestamps are nanoseconds and can be joined to the backend's
`input_source_ns`. CSV output is buffered and flushed every 30 frames and on
normal shutdown. Stop normally and retain both CSV files plus the existing
Mercury/runtime logs. If no detection pass ran, `detector.csv` may be absent;
this does not disable `tracking.csv`.

The CSV captures the active camera count, ROI at input and for the next frame,
ROI provenance (0=detector, 1=pose prediction), the keypoint model's actual
hand probability, mean/minimum XY and relative-depth confidence, optimizer
reprojection error/threshold, hand size, palm/wrist pose before rejection,
the ROI prediction interval/multiplier, and projected joints outside each view.
`model_ran=0` identifies an unavailable measurement; unavailable values are
`nan`, rather than stale data from a preceding frame.

`rejection_mask` is a bitmask; reasons can combine:

| Bit | Meaning |
| --- | --- |
| 1 | No input ROI |
| 2 | Hand boxes overlap |
| 4 | Keypoint model rejected the hand |
| 8 | Reprojection error exceeds the limit |
| 16 | Hand exceeds the distance limit |
| 32 | Predicted ROI is outside both cameras |
| 64 | Hand is still in the display warmup period |
| 128 | Quality filter rejected the keypoints or fitted pose |

Source review found two issues worth measuring before changing behavior:

- `history_timestamps` receives the current timestamp before
  `predict_new_regions_of_interest()`. Its `dt_now` is therefore zero on the
  normal path, so the nominal ROI prediction uses the last pose without
  extrapolation. The diagnostic fields record this directly.
- The overlap test uses `right_box.size_px` for the left box. The diagnostics
  record both the IoU used by current code and the IoU using the correct left
  box size. The rejection decision remains unchanged for the first test.

Tracking with only one active camera remains allowed, and the final
reprojection error measures 2D fit in the active camera(s). A low value does
not establish correct stereo depth when only one camera contributes.

Standalone logger verification (no OpenCV/ONNX dependencies required):

```bash
python3 backends/mercury_hand_tracking/tests/test_tracking_diagnostics.py
```

## Quality filter (enabled by default)

`mercury_quality_filter.patch` applies after the tracking diagnostics patch.
Both the normal installer and `rebuild_tracking_diagnostics.sh` handle this
order and copy `xr_mercury_quality_filter.hpp` from the overlay.

The filter is enabled when `MERCURY_XR_QUALITY_FILTER` is unset or empty.
The XREAL optical tracking test confirmed that the filter resolved the
reported pre-loss hand bounces. The thresholds remain unchanged.
Disable it explicitly in the shell launching XR Gate for comparison:

```bash
export MERCURY_XR_QUALITY_FILTER=0
```

The filter makes a decision independently for each hand:

- Before optimization, reject the hand if the mean XY keypoint confidence in
  any camera used for that hand is below `0.25` or is not finite.
- After optimization, reject the fitted pose if a camera's mean confidence is
  below `0.35` and the reprojection error exceeds `0.5`.
- Reject a non-finite fitted error while the filter is enabled.

Only the rejected hand becomes inactive. Its usual Mercury tracking history
is cleared and the existing detector/display warmup handles reacquisition.
The filter does not substitute a synthetic pose. Runtime prediction can then
use the preceding accepted optical samples according to its own settings.
Single-camera tracking remains allowed when its measurements pass these
checks. This filter does not crop the camera images or change the FOV.

Parameters are optional; defaults above are a starting point for XREAL tests:

```bash
export MERCURY_XR_QUALITY_MIN_MEAN_XY=0.25
export MERCURY_XR_QUALITY_WEAK_MEAN_XY=0.35
export MERCURY_XR_QUALITY_MAX_WEAK_REPROJECTION=0.5
```

The rebuilt library prints `[mercury_quality_filter] enabled=1` and its
thresholds. Keep `MERCURY_XR_TRACKING_DEBUG_CSV` set to record rejected frames
with bit `128` in `rejection_mask`. Unset `MERCURY_XR_QUALITY_FILTER` or set it
to `1` and restart the backend to restore filtering.

The default filter decisions were evaluated on the supplied October 12 CSV:
14 of 1060 previously published left-hand poses and 90 of 1472 right-hand
poses would be rejected. The two reported left-hand events are first rejected
at frames 718 and 2241. This is evaluation of recorded decisions; the live
tracker may follow a different path after rejection and must be tested again.

```bash
python3 backends/mercury_hand_tracking/tests/test_quality_filter.py
python3 backends/mercury_hand_tracking/tests/test_quality_filter.py /path/to/tracking.csv
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
