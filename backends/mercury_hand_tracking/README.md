# Mercury Hand Tracking Backend

Project-owned hand tracking backend using stereo input from `capture_service` and Monado/Mercury hand tracking components.

It publishes runtime-facing hand pose/joint data for `xr_runtime_adapter`, which then produces runtime hand/controller streams for OpenVR/Monado consumers.

## Main stream

```text
/tmp/tracking_streams.json : hand_tracking
```

## Current goal

Stable controller-like hand poses, left/right hand assignment, basic pinch/grab gestures, and safe runtime behavior. Full finger-perfect tracking is not the immediate target.

## Package output

```text
out/xreal_ultra/bin/backends/mercury_hand_tracking/
```

Upstream Monado/Mercury changes should be kept as patches under this backend, not as copied upstream source trees.
