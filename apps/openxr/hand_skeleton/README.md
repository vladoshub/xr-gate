# XR Gate Skeleton Viewer — Monado / OpenXR

A small C++17 app for inspecting hand joints and FRAME body trackers in Monado. The desktop window mirrors the headset preview or the left eye of a stereo view. Requires Linux, OpenGL 3.3, and X11 or XWayland.

By default, the app shows the **21 joints from XR Gate/Mercury** as exposed by Monado. Press `J` to show all **26 OpenXR joints**, including the palm and four estimated metacarpals. Both views use the poses returned by OpenXR without app-side offsets or smoothing.

## Build and run

Install dependencies on Ubuntu or Debian:

```bash
sudo apt install build-essential cmake pkg-config libopenxr-dev \
  libsdl2-dev libglew-dev libgl1-mesa-dev libx11-dev
```

```bash
XR_MONADO_HAND_TRACKING=1 bin/drivers/generic/monado_driver/start.sh
```

In another terminal, build and run the app:

```bash
bash apps/openxr/hand_skeleton/run.sh \
  --runtime bin/drivers/generic/monado_driver/openxr_monado_xrgate.json
```

Replace `generic` with your device package if needed. If `XR_RUNTIME_JSON` is already set, you can omit `--runtime`.

## FRAME body skeleton

Press `0` in `xr_client` to start FRAME, or set `RUN_FRAME_BODY_TRACKING=1` when starting XR Gate. Launch Monado from the runtime package after `runtime_body_trackers` appears so the driver can register the FRAME devices.

```bash
XR_MONADO_BODY_TRACKERS=1 XR_MONADO_HAND_TRACKING=1 XR_MONADO_DEVICE=xreal_ultra \
  ./devices/common/linux/scripts/monado_driver/start_monado_driver.sh
```

Run the viewer from the source checkout in a second terminal.

```bash
bash apps/openxr/hand_skeleton/run.sh \
  --runtime "$PWD/out/xr-gate/bin/drivers/xreal_ultra/monado_driver/openxr_monado_xrgate.json" \
  --body
```

The viewer requests the 15 FRAME joints and the waist tracker through the Monado `XR_MNDX_xdev_space` extension. The default preview displays a 3D model in a flat panel attached to the headset. Press `V` to switch to World view and press it again to return to the preview. World view renders the joints at their OpenXR LOCAL positions and presents both eye images in a stereo panel that follows the glasses.

When Monado exposes `XR_EXT_hand_tracking`, both views display the hand skeletons alongside FRAME. The `J`, `A` and `R` controls also apply to those hands.

Bones appear when both endpoint positions are valid. Cyan marks the left side, orange the right, and yellow a valid position that Monado reports as untracked. `FRAME LOST` means there are no valid joint positions for the current frame.

In World view, `IN VIEW L .. R ..` counts joint centers inside each eye's frustum. The stereo cameras can track joints outside the headset's display view. Press `P` to print the current joint positions in meters, OpenXR head-relative angles, and the distance between the ankles when both are valid. To compare frames, keep your feet still and take snapshots with the headset pointed down at different angles.

World view uses the eye poses when the runtime supplies an invalid head orientation. If the eye poses are also unavailable, the headset panel shows `STEREO POSE LOST` until valid eye poses return.

If the wrist fallback patch is installed, start XR Gate with `RUNTIME_BODY_TRACKER_FILTER_WRISTS=0` to include the FRAME wrists in the body stream and render the forearms.

## Controls

| Key | Action |
| --- | --- |
| `J` | Toggle between 21 and 26 joints |
| `A` | Show joint orientation axes |
| `R` | Toggle between OpenXR joint radii and fixed markers |
| `V` | Switch between the headset preview and world positions in body mode |
| `P` | Print one frame of live body positions and distances |
| `G` | Toggle the reference grid in world and hand views |
| `Esc` | Exit |

The left hand is cyan and the right hand is orange. Yellow marks a valid joint position that OpenXR reports as untracked. Invalid joints are hidden, and inactive hands show `LOST`.

Use `--demo` to render synthetic hands and `--body --demo` to render a synthetic body with hands in the desktop window.
