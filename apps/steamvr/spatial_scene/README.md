# SteamVR Spatial Scene

Experimental OpenVR scene application for rendering `runtime_spatial_proxy_mesh` as real stereo 3D geometry.

Unlike `spatial_overlay`, this app is a foreground OpenVR scene app and submits left/right eye textures through the OpenVR compositor. It is intended for geometry/depth validation, not for running transparently over another VR game.

## Package path

```text
out/xreal_ultra/bin/apps/steamvr/spatial_scene/
```

## Run from package

```bash
cd out/xreal_ultra
devices/xreal_ultra/linux/scripts/steamvr_spatial_scene/start_xr_steamvr_spatial_scene.sh
```

## Expected input

```text
/tmp/runtime_tracking_streams.json : runtime_spatial_proxy_mesh
```
