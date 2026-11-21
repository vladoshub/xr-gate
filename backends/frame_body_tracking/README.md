# FRAME body tracking

The Linux backend reads synchronized GRAY8 frames from the selected stereo
camera profile and runs the FRAME backbone on the CPU with ONNX Runtime 1.31.0
by default. It publishes positions for the 15
FRAME joints and a waist position from the midpoint of the hips in
`body_trackers`.

## Build

The regular XR Gate build and the Mercury component of the Linux split build
use the same ONNX Runtime SDK for both backends. To rebuild FRAME and the
runtime adapter in an existing package, run these commands from the repo root.
Use the package's existing `XR_BUILD_CPU` setting.

Set `XR_ONNXRUNTIME_VERSION=1.18.1` when building to select the legacy SDK.
See the [ONNX Runtime build settings](../../docs/ubuntu-builds.md#onnx-runtime-version)
for rebuilding both backends with the same version.

```bash
XR_BUILD_ONLY="frame_body_tracking xr_runtime_adapter" \
  ./devices/common/linux/scripts/build/install_build_deps_ubuntu.sh
XR_BUILD_ONLY="frame_body_tracking xr_runtime_adapter" \
  ./devices/common/linux/scripts/build/install_xr_gate_out.sh
```

The first AMD64 or ARM64 build downloads the FRAME backbone checkpoint, exports it on the CPU,
and installs it as `bin/body-tracking-models/frame/frame_backbone.onnx`. The
build cache keeps the export for subsequent builds. Set `XR_OUT_ROOT` to update
an existing runtime package. Use `XR_BUILD_ONLY=body_tracking_models` to
install just the model or `XR_DOWNLOAD_BODY_TRACKING_MODELS=0` to build the
backend binary separately. If you already have the original `backbone.ckpt`
from Edmond, set `XR_FRAME_CHECKPOINT` to its path when building.

The FRAME source checkout stays in `third_party/frame/` after the export.
Set `THIRD_PARTY_DIR` to use a different dependency directory. On the next
model build, an existing Git cache at `build/.dependency-cache/frame/source/`
moves to the new location if the destination is free.

GitHub builds publish `body-tracking-models-frame.tar.gz` separately. Place it
beside `unpack_xr_gate.sh` and the other release archives, or supply it with
`--frame-models`. The unpack helper also accepts the GitHub artifact ZIP.
For a local `riscv64` build, extract the model tarball into
`out/riscv64/xr-gate/bin` to install the pre-exported model.

## Run

```bash
cd out/xr-gate
./run_xr_client.sh --config xreal_ultra
```

Press `0` in `xr_client` to start or stop FRAME. Set
`RUN_FRAME_BODY_TRACKING=1` for automatic startup. The runtime adapter picks
up the SHM stream as soon as FRAME starts. An enabled application TCP server
includes body trackers as well.

FRAME uses the selected tracking profile's Basalt KB4 calibration, including
camera ordering, extrinsics and time offset. `XR_FRAME_CALIB` and
`XR_FRAME_MODEL` override the automatically resolved paths. The backend log
reports both paths, the configured angles, and the count of frames where the
pitch gate suppressed the lower body.

Wait for `runtime_body_trackers`, then launch Monado from a second terminal
in the package so it can enumerate the tracker devices. Enable both flags
to see hands and body together in the viewer.

```bash
XR_MONADO_BODY_TRACKERS=1 XR_MONADO_HAND_TRACKING=1 XR_MONADO_DEVICE=xreal_ultra \
  ./devices/common/linux/scripts/monado_driver/start_monado_driver.sh
```

## Camera selection

FRAME reads the ordered stereo pair advertised by `capture_service_cpp` in
the selected SHM registry. The publisher takes the main pair's stream IDs
from `camera.output.left_stream` and `camera.output.right_stream`. Legacy
registries use `camera0` and `camera1` as the fallback.

To select cameras specifically for FRAME, set `XR_FRAME_CAPTURE_REGISTRY`
in the tracking device's `frame_body_tracking.env`. The main registry is
the default. Additional views from the main source default to
`/tmp/capture_service_streams_additional.json`, and the extra capture worker
defaults to `/tmp/capture_service_extra_streams.json` on Linux.

FRAME selects `primary` or the sole advertised pair by default. Select a
named pair with `XR_FRAME_STEREO_PAIR`. Specify `XR_FRAME_CAM0_STREAM` and
`XR_FRAME_CAM1_STREAM` in calibration order when using individually published
views without a declared pair. Both streams must publish calibrated `GRAY8`
views with the correct image dimensions and share a timestamp domain. FRAME
matches frames within 1 ms, including when their sequence numbers differ.
Set `XR_FRAME_CALIB` to match the selected camera geometry and transformations
when the Basalt cameras use a different pair.

## Tracking

The fisheye remap uses a 180-degree roll and the selected downward pitch from
each original camera. With the default filters, both 3D estimates must fall
inside the input view, have plausible distances from the cameras and agree
in the IMU frame. Tracker confidence describes agreement between the two
estimates. Missing estimates retain their tracker IDs and report `LOST`.

With camera pitch checking enabled, FRAME checks both physical cameras before
calling `model.infer()`. If either camera is above the configured pitch relative
to the horizon, inference is skipped and all tracker IDs report `LOST`.
This uses the Basalt pose at exposure and the selected camera-to-IMU extrinsics.
Inference is also skipped if no valid time-matched Basalt pose arrives for
this check within the configured pose wait.
The ONNX session stays loaded, so tracking resumes on the next eligible frame
without reloading the model. The runtime adapter may continue predicting during
its configured hold and loss windows.

Set `XR_FRAME_MAX_CAMERA_PITCH_DEG=-1` to run inference at any camera pitch.
This disables only the pitch gate; the other configured observation filters
still apply. `XR_FRAME_CHECK_CAMERA_PITCH=0` also disables this gate.

The backend matches the exposure timestamp to the Basalt pose ring after
applying the calibration's camera-to-IMU time offset. It uses the exact pose
or interpolates between two tracking samples on the same side of a Basalt
reset. The runtime adapter applies the Basalt HMD coordinate transform and
runtime origin to the published positions.

| Environment variable | Default | Effect |
| --- | --- | --- |
| `XR_FRAME_PITCH_DOWN_DEG` | `0` | Downward pitch of the virtual cameras |
| `XR_FRAME_MAX_CAMERA_PITCH_DEG` | `0` (`-50` for XREAL Ultra cameras) | Highest pitch of either physical camera for inference; `-1` disables the pitch gate |
| `XR_FRAME_THREADS` | `4` | ONNX Runtime intra-op CPU threads |
| `XR_FRAME_MAX_FPS` | `20` | Inference rate limit, with `0` using every available latest pair |
| `XR_FRAME_MAX_STEREO_DISAGREEMENT_M` | `0.24` | Maximum 3D disagreement between the two camera estimates |

### Observation filter settings

The selected tracking device's `frame_body_tracking.env` configures the filters.
For XREAL Ultra, edit `devices/xreal_ultra/frame_body_tracking.env`. Environment
overrides also work when starting `xr_client`. Restart FRAME to apply changes.

| Environment variable | Default | Effect |
| --- | --- | --- |
| `XR_FRAME_CHECK_FIELD_OF_VIEW` | `1` | Require the joint's 2D estimate to be inside both physical camera images |
| `XR_FRAME_CHECK_POSITIVE_DEPTH` | `1` | Require `Z > 0` in each virtual camera's 3D joint estimate |
| `XR_FRAME_CHECK_JOINT_DISTANCE` | `1` | Apply the per-joint distance ranges below |
| `XR_FRAME_CHECK_STEREO_DISAGREEMENT` | `1` | Apply `XR_FRAME_MAX_STEREO_DISAGREEMENT_M` |
| `XR_FRAME_CHECK_CAMERA_PITCH` | `1` | Skip inference above `XR_FRAME_MAX_CAMERA_PITCH_DEG`; ignored when the threshold is `-1` |

Set a check to `0` to disable it. The equivalent executable options are
`--check-field-of-view`, `--check-positive-depth`, `--check-joint-distance`,
`--check-stereo-disagreement` and `--check-camera-pitch`, each taking `0` or `1`.

Each joint has `XR_FRAME_<JOINT>_MIN_M` and `XR_FRAME_<JOINT>_MAX_M` settings.
The ranges measure distance from each camera before combining the two estimates.

| Joint suffix | Minimum (m) | Maximum (m) |
| --- | --- | --- |
| `NECK` | 0.06 | 0.75 |
| `LEFT_SHOULDER`, `RIGHT_SHOULDER` | 0.16 | 1.05 |
| `LEFT_ELBOW`, `RIGHT_ELBOW` | 0.28 | 1.40 |
| `LEFT_WRIST`, `RIGHT_WRIST` | 0.18 | 1.55 |
| `LEFT_HIP`, `RIGHT_HIP` | 0.25 | 1.80 |
| `LEFT_KNEE`, `RIGHT_KNEE` | 0.30 | 2.20 |
| `LEFT_FOOT`, `RIGHT_FOOT` | 0.35 | 2.60 |
| `LEFT_TOE`, `RIGHT_TOE` | 0.35 | 2.70 |

For example, `XR_FRAME_LEFT_SHOULDER_MIN_M=0.10` lowers the left shoulder's
minimum distance to 10 cm. The matching executable option is
`--left-shoulder-min-m 0.10`. Each range must have finite values with
`0 <= MIN_M <= MAX_M`. The waist is derived from both accepted hip positions.

Set `XR_FRAME_CHECK_POSITIVE_DEPTH=0` to accept joint estimates with zero or
negative camera depth. Field-of-view, distance and stereo disagreement checks
remain independent and apply according to their own settings.

Finite coordinates and valid estimates from both cameras are required for
a joint observation. A valid time-matched Basalt pose is
required to publish its world position. When stereo disagreement checking is
disabled, accepted observations receive `confidence=1`.
