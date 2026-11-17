# FRAME body tracking

The Linux backend reads synchronized GRAY8 frames from the selected stereo
camera profile and runs the FRAME backbone on the CPU with ONNX Runtime 1.18.1. It publishes
positions for the 15 FRAME joints and a waist position from the midpoint of
the hips in `body_trackers`.

## Build

The regular XR Gate build and the Mercury component of the Linux split build
use the same ONNX Runtime SDK for both backends. To rebuild FRAME and the
runtime adapter in an existing package, run these commands from the repo root.
Use the package's existing `XR_BUILD_CPU` setting.

```bash
XR_BUILD_ONLY="frame_body_tracking xr_runtime_adapter" \
  ./devices/common/linux/scripts/build/install_build_deps_ubuntu.sh
XR_BUILD_ONLY="frame_body_tracking xr_runtime_adapter" \
  ./devices/common/linux/scripts/build/install_xr_gate_out.sh
```

The first build downloads the FRAME backbone checkpoint, exports it on the CPU,
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
each original camera. Both 3D estimates must fall inside the input view, have
plausible distances from the cameras and agree in the IMU frame. Tracker
confidence describes agreement between the two estimates. Missing estimates
retain their tracker IDs and report `LOST`.

FRAME reports hips, legs and waist as `LOST` when either physical camera is
above the configured pitch relative to the horizon. This uses the Basalt
pose at exposure and the selected camera-to-IMU extrinsics. The runtime
adapter may continue predicting during its configured hold and loss windows.

The backend matches the exposure timestamp to the Basalt pose ring after
applying the calibration's camera-to-IMU time offset. It uses the exact pose
or interpolates between two tracking samples on the same side of a Basalt
reset. The runtime adapter applies the Basalt HMD coordinate transform and
runtime origin to the published positions.

| Environment variable | Default | Effect |
| --- | --- | --- |
| `XR_FRAME_PITCH_DOWN_DEG` | `0` (`20` for XREAL Ultra cameras) | Downward pitch of the virtual cameras |
| `XR_FRAME_MAX_CAMERA_PITCH_DEG` | `0` (`-40` for XREAL Ultra cameras) | Highest pitch of the physical cameras for hips, legs and waist tracking |
| `XR_FRAME_THREADS` | `4` | ONNX Runtime intra-op CPU threads |
| `XR_FRAME_MAX_FPS` | `20` | Inference rate limit, with `0` using every available latest pair |
| `XR_FRAME_MAX_STEREO_DISAGREEMENT_M` | `0.24` | Maximum 3D disagreement between the two camera estimates |
