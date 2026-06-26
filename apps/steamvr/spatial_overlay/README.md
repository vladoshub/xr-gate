# SteamVR Spatial Overlay

Optional OpenVR overlay app for visualizing `runtime_spatial_proxy_mesh` in SteamVR.

It reads the runtime spatial proxy mesh from SHM and renders it as a transparent HMD overlay. It supports mesh, wireframe, points, and stereo/SBS overlay modes.

This is useful for passthrough/debug visualization over SteamVR content. It is not a true compositor-level depth occlusion implementation.

## Package path

```text
out/xreal_ultra/bin/apps/steamvr/spatial_overlay/
```

## Run from package

```bash
cd out/xreal_ultra
devices/xreal_ultra/linux/scripts/steamvr_spatial_overlay/start_xr_steamvr_spatial_overlay.sh
```

## Expected input

```text
/tmp/runtime_tracking_streams.json : runtime_spatial_proxy_mesh
```
