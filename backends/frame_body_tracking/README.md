# FRAME body tracking

The Linux backend reads synchronized GRAY8 frames from XREAL Air 2 Ultra and
runs the FRAME backbone on the CPU with ONNX Runtime 1.18.1. It publishes
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

## Run

Use the exported `frame_backbone.onnx` with inputs `images`, `K`, `d` and outputs
`uv`, `xyz`. The model is supplied through `XR_FRAME_MODEL`. Use the same
Basalt KB4 calibration as the capture profile, including its camera ordering,
rotation and time offset.

```bash
cd out/xr-gate
RUN_FRAME_BODY_TRACKING=1 \
XR_FRAME_MODEL="$HOME/frame-onnx-test/frame_backbone.onnx" \
XR_FRAME_CALIB="$HOME/frame-onnx-test/ultra.json" \
  ./run_xr_client.sh --config xreal_ultra
```

The launcher enables the SHM body tracker input and output in the runtime
adapter. An enabled application TCP server includes body trackers as well.
The backend log reports the frame count, number of tracked joints, pose misses
and inference time. Launch Monado from a second terminal in the package to
enumerate the tracker devices there.

```bash
XR_MONADO_BODY_TRACKERS=1 XR_MONADO_DEVICE=xreal_ultra \
  ./devices/common/linux/scripts/monado_driver/start_monado_driver.sh
```

## Tracking

The fisheye remap uses a 180-degree roll and a 20-degree downward pitch from
each original camera. Both 3D estimates must fall inside the input view, have
plausible distances from the cameras and agree in the IMU frame. Tracker
confidence describes agreement between the two estimates. Missing estimates
retain their tracker IDs and report `LOST`.

The backend matches the exposure timestamp to the Basalt pose ring after
applying the calibration's camera-to-IMU time offset. It uses the exact pose
or interpolates between two tracking samples on the same side of a Basalt
reset. The runtime adapter applies the Basalt HMD coordinate transform and
runtime origin to the published positions.

| Environment variable | Default | Effect |
| --- | --- | --- |
| `XR_FRAME_PITCH_DOWN_DEG` | `20` | Downward pitch of the virtual cameras |
| `XR_FRAME_THREADS` | `4` | ONNX Runtime intra-op CPU threads |
| `XR_FRAME_MAX_FPS` | `20` | Inference rate limit, with `0` using every available latest pair |
| `XR_FRAME_MAX_STEREO_DISAGREEMENT_M` | `0.24` | Maximum 3D disagreement between the two camera estimates |
