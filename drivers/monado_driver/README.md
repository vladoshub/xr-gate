# Monado Driver Integration

Project-owned Monado driver integration for consuming runtime streams from `xr_runtime_adapter`.

The driver should not talk directly to capture, Basalt, Mercury, or device hardware. It should consume runtime-normalized streams only.

## Expected runtime streams

```text
/tmp/runtime_tracking_streams.json : runtime_hmd_pose
/tmp/runtime_tracking_streams.json : runtime_controller_state
/tmp/runtime_tracking_streams.json : runtime_hand_tracking        # optional fallback
```

## Package output

```text
out/xreal_ultra/bin/drivers/monado_driver/
```

Monado is upstream code. Required changes to Monado should be represented as patches under `drivers/monado_driver/patches/`.
