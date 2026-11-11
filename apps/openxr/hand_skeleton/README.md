# XR Gate Hand Skeleton — Monado / OpenXR

A small C++17 app for inspecting hand joint positions and orientations in Monado. It renders in stereo in the headset and mirrors the left eye in a desktop window. Requires Linux, OpenGL 3.3, and X11 or XWayland.

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

## Controls

| Key | Action |
| --- | --- |
| `J` | Toggle between 21 and 26 joints |
| `A` | Show joint orientation axes |
| `R` | Toggle between OpenXR joint radii and fixed markers |
| `G` | Toggle the reference grid |
| `Esc` | Exit |

The left hand is cyan and the right hand is orange. Yellow marks a valid joint position that OpenXR reports as untracked. Invalid joints are hidden, and inactive hands show `LOST`.

To check rendering without Monado or cameras, run `bash apps/openxr/hand_skeleton/run.sh --demo`. The demo uses synthetic hands. Build and rendering were verified with a test runtime. Physical headset testing is pending.