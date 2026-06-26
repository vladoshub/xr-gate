# XR Bridges

Transport bridge tools for capture, runtime tracking, and spatial mesh streams.

These are not device backends and not runtime adapters. They republish existing streams over network/debug transports.

## Main tools

```text
capture_net_bridge
tracking_udp_bridge
tracking_udp_debug_receiver
spatial_proxy_mesh_udp_debug_receiver
```

## Package output

```text
out/xreal_ultra/bin/bridges/
```

Use these for diagnostics, remote streaming, and UDP validation without changing the core runtime pipeline.
