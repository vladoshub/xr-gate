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

Start XR Gate with `RUN_FRAME_BODY_TRACKING=1` and wait for the `runtime_body_trackers` stream. Launch Monado from `out/xr-gate` after the stream appears so the driver can register the FRAME devices.

```bash
XR_MONADO_BODY_TRACKERS=1 XR_MONADO_DEVICE=xreal_ultra \
  ./devices/common/linux/scripts/monado_driver/start_monado_driver.sh
```

Run the viewer from the source checkout in a second terminal.

```bash
bash apps/openxr/hand_skeleton/run.sh \
  --runtime "$PWD/out/xr-gate/bin/drivers/xreal_ultra/monado_driver/openxr_monado_xrgate.json" \
  --body
```

The viewer requests the 15 FRAME joints and the waist tracker through the Monado `XR_MNDX_xdev_space` extension. The default preview displays a 3D model in a flat panel attached to the headset. The panel follows the glasses as you tilt them down to bring the legs into camera view. Press `V` in the viewer window to inspect the joints in stereo at their OpenXR LOCAL positions and press it again to return to the preview.

Bones appear when both endpoint positions are valid. Cyan marks the left side, orange the right, and yellow a valid position that Monado reports as untracked. `FRAME LOST` means there are no valid joint positions for the current frame.

If the wrist fallback patch is installed, start XR Gate with `RUNTIME_BODY_TRACKER_FILTER_WRISTS=0` to include the FRAME wrists in the body stream and render the forearms.

## Controls

| Key | Action |
| --- | --- |
| `J` | Toggle between 21 and 26 joints |
| `A` | Show joint orientation axes |
| `R` | Toggle between OpenXR joint radii and fixed markers |
| `V` | Switch between the headset preview and world positions in body mode |
| `G` | Toggle the reference grid in world and hand views |
| `Esc` | Exit |

The left hand is cyan and the right hand is orange. Yellow marks a valid joint position that OpenXR reports as untracked. Invalid joints are hidden, and inactive hands show `LOST`.

Use `--demo` to render synthetic hands and `--body --demo` to render a synthetic body in the desktop window.
