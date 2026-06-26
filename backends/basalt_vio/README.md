# Basalt VIO Backend

Project-owned Basalt-facing backend for 6DoF HMD tracking.

It consumes stereo camera and IMU streams from `capture_service`, runs Basalt VIO, and publishes the source HMD pose stream used by `xr_runtime_adapter`.

## Main stream

```text
/tmp/tracking_streams.json : hmd_pose
```

## Package output

```text
out/xreal_ultra/bin/backends/basalt_vio/
```

## Notes

Basalt itself is upstream/third-party code. Project-owned integration files live here; modifications to upstream Basalt should be stored as patches, not copied as full upstream files.
