# XR Video Backend

Optional backend for stereo video passthrough.

It reads synchronized stereo frames from `capture_service` and republishes them as an XR video stream for `xr_runtime_adapter` or video overlay consumers.

## Package output

```text
out/xreal_ultra/bin/backends/xr_video/
```

## Typical path

```text
capture_service -> xr_video_backend -> xr_runtime_adapter -> runtime_stereo_video / SteamVR overlay
```

This backend is separate from tracking and should remain optional.
