# XR Spatial Backend

Optional backend for live stereo depth, spatial proxy mesh, and lightweight backend-only scan experiments.

It reads stereo frames from `capture_service`, optionally reads HMD pose, computes stereo depth, publishes an organized-grid spatial proxy mesh, and can save simple scan artifacts to disk.

## Main streams

```text
/tmp/runtime_tracking_streams.json : spatial_proxy_mesh
/tmp/runtime_tracking_streams.json : runtime_spatial_summary
```

## Common modes

```text
SPATIAL_POSE_INPUT=shm   # 6DoF/world-space mesh and scanner
SPATIAL_POSE_INPUT=none  # camera-relative live passthrough / 3DoF debug mode
```

## Package output

```text
out/xreal_ultra/bin/backends/xr_spatial/
```

`xr_spatial` is optional and must not sit between tracking backends and `xr_runtime_adapter`.

## Automatic capture profiles and camera-only mode

The launcher reads the top-level `profile` value from
`/tmp/capture_service_streams.json` and loads the exact matching file from
`configs/profiles/<profile>.env`. Explicit `XR_SPATIAL_PROFILE` or
`CAPTURE_PROFILE` remains higher priority. When the field is absent, the
existing `xreal_air2ultra_unified_480` profile is used.

`xr_spatial` consumes stereo frames and an optional HMD pose stream; it does not
consume raw `imu0`. For camera-coordinate live depth, a profile can set:

```bash
SPATIAL_POSE_INPUT=none
SPATIAL_MAP_FRAME=camera
```

The existing Basalt-style calibration JSON with `value0.T_imu_cam` remains
supported unchanged. A stereo-only JSON may instead contain
`value0.T_cam1_cam0`, using the convention:

```text
X_cam1 = T_cam1_cam0 * X_cam0
```

It must still contain two entries in `value0.intrinsics` and
`value0.resolution`.
