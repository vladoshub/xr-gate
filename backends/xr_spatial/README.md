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
