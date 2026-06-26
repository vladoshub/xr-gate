# Shared Include Tree

Shared project-owned headers for capture, tracking, runtime, video, and spatial contracts.

This tree is organized by protocol/domain, not by backend implementation.

## Main namespaces

```text
capture_client/   capture service contracts and SHM/TCP readers
xr_tracking/      source tracking contracts and publishers
xr_runtime/       runtime-normalized contracts and publishers
xr_video/         stereo video contracts/control/shm helpers
xr_spatial/       spatial proxy mesh contracts and publishers
```

Backends and drivers should depend on these shared contracts instead of copying ABI structs locally.
