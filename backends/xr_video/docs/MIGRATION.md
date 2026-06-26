# XR Video migration notes

Old local build output under `backends/xr_video/build/` should be removed from source control.

Canonical build/install paths:

```text
build/backends/xr_video/relwithdebinfo/
bin/backends/xr_video/xr_video_backend
bin/backends/xr_video/xr_video_monitor
```

The install script does not clone third_party code. It checks for CLI11 in the existing Basalt/vcpkg install tree, normally produced by building `backends/basalt_vio`.
