# xr_runtime_adapter

`xr_runtime_adapter` is the runtime-side bridge for XR Gate. It reads backend tracking streams, applies runtime coordinate transforms and stability filters, then republishes normalized runtime streams for drivers and tools.

## What it does

* Reads HMD pose from backend tracking streams.
* Reads hand tracking frames from Mercury or compatible hand backends.
* Optionally reads controller input override streams.
* Optionally reads body tracker streams.
* Applies runtime transform configuration.
* Applies hand stability, reacquire, lost-pose hold/prediction, and gesture derivation.
* Publishes runtime SHM streams consumed by OpenVR/Monado drivers and debug tools.

Typical data flow:

```text
backend streams
  -> xr_runtime_adapter
  -> runtime streams
  -> OpenVR / Monado / debug viewers
```

## Main runtime streams

Common output streams:

```text
runtime_pose
runtime_hand_tracking
runtime_controller_state
runtime_body_trackers
runtime_spatial_proxy_mesh
runtime_spatial_summary
```

Runtime streams are usually registered in:

```text
/tmp/runtime_tracking_streams.json
```

Source/backend streams are usually registered in:

```text
/tmp/tracking_streams.json
```

## Build

From the repository root:

```bash
runtime_adapters/xr_runtime_adapter/scripts/linux/install_xr_runtime_adapter.sh
```

## Run

Default SHM runtime adapter launcher:

```bash
runtime_adapters/xr_runtime_adapter/scripts/linux/start_xr_runtime_adapter_shm.sh
```

The script is controlled mostly through environment variables, so individual runtime features can be enabled without changing code.

## Hand tracking stability

Hand stability gate can smooth short tracking losses and reject unstable reacquire jumps.

Useful settings:

```bash
RUNTIME_HAND_STABILITY_GATE=1
RUNTIME_HAND_GATE_HOLD_LOST_MS=50
RUNTIME_HAND_GATE_PREDICT_LOST_MS=350
RUNTIME_HAND_GATE_PREDICTION_DAMPING=0.5
RUNTIME_HAND_GATE_MAX_PREDICTION_VELOCITY_MPS=2.0
RUNTIME_HAND_GATE_REACQUIRE_BLEND_MS=0
```

For immediate inertial prediction after hand loss:

```bash
RUNTIME_HAND_GATE_HOLD_LOST_MS=0
RUNTIME_HAND_GATE_PREDICT_LOST_MS=400
RUNTIME_HAND_GATE_PREDICTION_DAMPING=0.8
```

For safer behavior with less hand drift:

```bash
RUNTIME_HAND_GATE_HOLD_LOST_MS=100
RUNTIME_HAND_GATE_PREDICT_LOST_MS=300
RUNTIME_HAND_GATE_PREDICTION_DAMPING=0.35
RUNTIME_HAND_GATE_MAX_PREDICTION_VELOCITY_MPS=0.8
```

## Derived gestures

The adapter can ignore backend-provided hand gestures and derive runtime gestures from hand pose data.

```bash
RUNTIME_IGNORE_BACKEND_HAND_GESTURES=1
RUNTIME_DERIVE_HAND_GESTURES=1
```

Common grab tuning:

```bash
DERIVED_GRAB_ACTIVE_THRESHOLD=0.99
DERIVED_GRAB_DEACTIVE_THRESHOLD=0.65
DERIVED_GRAB_RESPONSE_START=0.85
```

## Controller input override

The adapter can merge hand tracking with external controller input. This is useful when hand pose is available, but buttons, triggers, sticks, or fallback input should come from an external controller.

Typical stream:

```text
controller_input -> runtime_controller_state
```

The resulting runtime controller state can be consumed by the OpenVR driver.

## Body trackers

Body tracker input can be enabled through SHM or UDP, then republished as runtime body trackers.

Example:

```bash
BODY_TRACKERS_INPUT=shm
PUBLISH_RUNTIME_BODY_TRACKERS=1
```

Optional body tracker stability gate:

```bash
RUNTIME_BODY_TRACKER_STABILITY_GATE=1
RUNTIME_BODY_TRACKER_HOLD_LOST_MS=150
RUNTIME_BODY_TRACKER_PREDICT_LOST_MS=350
RUNTIME_BODY_TRACKER_MAX_PREDICTION_VELOCITY_MPS=0.8
RUNTIME_BODY_TRACKER_PREDICTION_DAMPING=0.30
```

This keeps short body tracker losses from immediately dropping the tracker. Prediction is applied per tracker, not to the whole body set.

## Spatial proxy mesh

The adapter can receive spatial proxy mesh data from `spatial_mapper` and republish it in runtime coordinates.

Typical flow:

```text
spatial_proxy_mesh
  -> xr_runtime_adapter
  -> runtime_spatial_proxy_mesh
```

This is intended for runtime debug visualization and future spatial collision/overlay experiments.

## Debugging

Inspect registered streams:

```bash
cat /tmp/tracking_streams.json
cat /tmp/runtime_tracking_streams.json
```

Enable hand gate CSV debug if supported by the current build:

```bash
RUNTIME_HAND_GATE_DEBUG_CSV=/tmp/runtime_hand_gate.csv \
runtime_adapters/xr_runtime_adapter/scripts/linux/start_xr_runtime_adapter_shm.sh

tail -f /tmp/runtime_hand_gate.csv
```

Useful things to check:

```text
stream exists
stream frame counter increases
stream age stays low
hand status does not immediately become lost
runtime_controller_state still receives hand/controller data
```

## Notes

* The adapter should own runtime coordinate correction.
* Debug viewers should usually run without applying an additional transform, otherwise coordinates may be transformed twice.
* Most experimental features are opt-in through environment variables.
* Defaults should preserve the stable runtime path unless a feature is explicitly enabled.