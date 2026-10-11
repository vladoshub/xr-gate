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
out/xr-gate/bin/backends/mercury_hand_tracking/
```

Upstream Monado/Mercury changes should be kept as patches under this backend, not as copied upstream source trees.

## Optical velocity

The camera consumer derives raw palm/controller linear and angular velocity
from source camera timestamps. Duplicate/out-of-order timestamps do not replace
the derivative anchor, and a reset counter change starts a new history.

With the runtime hand gate enabled, published optical velocity is recalculated
from accepted poses. Rejected detections and held/predicted output are excluded.
The existing prediction-window mode also smooths the published linear estimate;
otherwise it uses consecutive accepted measurements. Real optical loss clears
this measurement history, so the first returning pose has no optical velocity.
Reacquire blending uses these accepted velocity targets. The continuity speed
limit caps published optical linear velocity; angular velocity is capped at
30 rad/s. Camera-local motion is subsequently transformed with HMD motion by
the runtime adapter.

To rebuild just the camera consumer after a wrapper/header change:

```bash
ROOT_PROJECT="$PWD" bash backends/mercury_hand_tracking/scripts/linux/build_capture_hand_tracking_backend.sh
```

This installs `capture_hand_tracking_backend` under `bin/backends/mercury_hand_tracking`.
