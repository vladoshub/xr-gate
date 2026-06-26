# SteamVR Stereo Video Overlay

Optional OpenVR overlay app for displaying runtime stereo video in SteamVR.

It is separate from the OpenVR driver. The driver owns HMD/controllers/display integration; this app only creates an OpenVR overlay and uploads the latest stereo/SBS frame.

## Package path

```text
out/xreal_ultra/bin/apps/steamvr/video_overlay/
```

## Run from package

```bash
cd out/xreal_ultra
devices/xreal_ultra/linux/scripts/steamvr_video_overlay/start_steamvr_video_overlay.sh
```

## Expected path

```text
capture_service -> xr_video_backend -> xr_runtime_adapter -> steamvr_video_overlay
```
