# SteamVR on Linux: universal launcher

Start `./run_xr_client.sh` in another terminal and keep it running. Enable the
headset's physical SBS display mode at the desired resolution and refresh rate.
From the release's `xr-gate` directory, run:

```bash
XR_OPENVR_DISPLAY_MODE=extended_sbs \
XR_DISPLAY_FREQUENCY_HZ=60 \
./run_openvr.sh
```

The same launcher works with any installed XR Gate device profile. Neither a GPU
vendor nor a connector name is required. It preserves the desktop's GPU selection
and the saved driver window position. With several device profiles installed, it
uses the currently registered profile when unambiguous; otherwise specify
`XR_OPENVR_DEVICE=<profile>`.

The launcher prefers an `extended_sbs` driver package at the selected frequency.
If only a direct package at that frequency exists, it registers that package and
overrides the display settings. It never silently substitutes another frequency
or device. No driver rebuild is necessary for this mode switch.

Steam is closed gracefully before updating settings. If it does not exit, the
launcher stops and asks you to close it. Existing settings are backed up once as
`steamvr.vrsettings.before-xr-gate`; unrelated runtime/calibration settings are kept.

The windowed path sets:

```json
{
  "xr_tracking": {"isDisplayOnDesktop": true},
  "steamvr": {"directMode": false, "allowFallbackMirrorWindowLinux": true},
  "direct_mode": {"enable": false}
}
```

This enables the Linux compositor's window fallback when direct display
acquisition fails. A stereo window is not a guarantee of Direct Mode's latency,
refresh synchronization, or exclusive presentation. Tested on Ubuntu 26.04/X11
with NVIDIA and XREAL Air 2 Ultra at 3840×1080/60 Hz.

## Optional display and GPU selection

On **X11**, an explicit output lets the launcher enable the display, set its mode
and rate, and align the compositor window with its actual desktop coordinates:

```bash
xrandr --query
XR_OPENVR_DISPLAY_MODE=extended_sbs \
XR_DISPLAY_FREQUENCY_HZ=60 \
XR_OPENVR_OUTPUT=DP-2 \
./run_openvr.sh
```

Replace `DP-2` with your connected output. Resolution comes from the selected
driver; `XR_OPENVR_DGPU_MODE=<width>x<height>` overrides it. Without an output,
the launcher does not change physical display settings: configure the actual
headset refresh rate yourself to match `XR_DISPLAY_FREQUENCY_HZ`.

Use `XR_OPENVR_WINDOW_X` and `XR_OPENVR_WINDOW_Y` for explicit window coordinates.
If needed, `XR_OPENVR_GPU_VENDOR=nvidia` or `amd` restricts Vulkan to that vendor;
the default `auto` leaves GPU selection unchanged. Vendor selection does not
distinguish between two GPUs from the same vendor. `XR_OPENVR_VK_ICD_FILENAMES`
can point to an explicit ICD manifest. The older `XR_OPENVR_DGPU_VENDOR` and
`XR_OPENVR_DGPU_OUTPUT` names are accepted too.

`XR_OPENVR_LAUNCH_MODE=steam` opens only Steam; `none` applies settings without
launching. The default is `steamvr`.

## Direct Mode

```bash
XR_OPENVR_DISPLAY_MODE=direct XR_DISPLAY_FREQUENCY_HZ=60 ./run_openvr.sh
```

This turns off the window fallback again. If an X11 output is specified, it is
marked `non-desktop=1` and disabled from the desktop. Without an output, physical
display preparation is left to the system. The existing
`run_openvr_dgpu_direct.sh` remains available for its established dGPU setup.

## Wayland

Windowed SBS on Wayland is **experimental and not hardware-verified**. The
launcher allows it without running `xrandr` or requiring a connector. SteamVR may
present its fallback window through XWayland; actual operation and window
placement depend on SteamVR, the graphics driver, and your Wayland compositor.
The successful X11 test does not establish Wayland compatibility.

Enable the headset display and choose its resolution, refresh rate, and window
placement in your compositor's settings or rules. Omit `XR_OPENVR_OUTPUT` and
`XR_OPENVR_DGPU_OUTPUT`: `xrandr` cannot configure physical Wayland outputs.
Saved X11 window coordinates may not be honored by the Wayland compositor.

For **Direct Mode**, Wayland support is a separate question: SteamVR needs the
compositor/driver's DRM leasing support. See Valve's
[Linux requirements](https://help.steampowered.com/en/faqs/view/18A4-1E10-8A94-3DDA).
Do not interpret the experimental windowed option as a promise of Direct Mode
support on every Wayland desktop.
