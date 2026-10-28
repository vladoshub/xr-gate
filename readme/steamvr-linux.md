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

The launcher prefers a package at the selected frequency and display mode. If
none exists, it reuses a package for the same device and overrides its runtime
settings. A `60HZ` package can therefore run at 90, 72, or 59.94 Hz without
rebuilding the driver or changing the package files. If several fallback
packages exist, the selected display mode is preferred, then the lowest default
frequency. `DRIVER_PACKAGE=<path/to/xr_tracking>` selects a package explicitly.
The physical display must support and use the requested resolution and rate.

Steam is closed gracefully before updating settings. If it does not exit, the
launcher stops and asks you to close it. Existing settings are backed up once as
`steamvr.vrsettings.before-xr-gate`; unrelated runtime/calibration settings are kept.
The driver frequency and SteamVR's `preferredRefreshRate` are both set to the
selected rate. The latter controls the requested display mode; SteamVR can still
report a different runtime frequency, so this does not guarantee correct timing.

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

### Interactive display setup (X11)

Recommended before starting SteamVR to avoid display mode and placement problems:

```bash
./configure_openvr_display.sh 3840x1080     # defaults to 60 Hz
./configure_openvr_display.sh 3840x1080 90  # optional explicit rate
```

Enable the headset's SBS mode first. Choose the reference monitor (e.g. `ARZOPA`),
then the headset (e.g. `Air 2 Ultra`), using its EDID name, connector or list number.
The reference monitor becomes primary at `0,0`; the headset is placed to its right
at the requested resolution and rate. Ambiguous names require a connector or
number. The script checks advertised modes before changing displays and prints
the `run_openvr.sh` command with the selected output, resolution and frequency
to align the SteamVR window with its new position.

Works with AMD and NVIDIA through X11 RandR; requires `python3` and `xrandr`
(`x11-xserver-utils` on Ubuntu/Debian). It does not require `xdotool`, root,
`nvidia-settings`, or a driver rebuild. On Wayland, use the desktop display
settings instead; XWayland cannot configure physical outputs this way.

### Explicit display and GPU overrides

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

On AMD, the default `auto` also needs no vendor flag. Install the system's Mesa
Vulkan driver (RADV). Explicit `XR_OPENVR_GPU_VENDOR=amd` discovers installed
`radeon_icd*.json` manifests, retaining both 32-bit and 64-bit variants when
present. Explicit `VK_DRIVER_FILES` takes precedence over `VK_ICD_FILENAMES`.
For several AMD GPUs, `XR_OPENVR_DRI_PRIME` can select a GPU with the `amd`
override; vendor selection alone cannot distinguish them.

`XR_OPENVR_LAUNCH_MODE=steam` opens only Steam; `none` applies settings without
launching. The default is `steamvr`.

## Direct Mode

```bash
XR_OPENVR_DISPLAY_MODE=direct XR_DISPLAY_FREQUENCY_HZ=60 ./run_openvr.sh
```

On **X11**, choose the headset display from the list on the first launch. The
launcher sets `non-desktop=1` and removes that output from the desktop before
starting SteamVR. SteamVR then attempts exclusive display acquisition; this
preparation alone does not guarantee DRM/Vulkan support from the GPU driver.
No window-moving helper or `xdotool` is needed for Direct Mode.

The choice is remembered per device using the display's EDID, so a connector
rename does not select an unrelated monitor. Run with `--choose-output` to choose
again, or set `XR_OPENVR_OUTPUT=DP-2` explicitly. Without EDID, choose each time
or use an explicit output. A first launch without a terminal requires that
explicit output. Use the launcher for each Direct Mode start to prepare the
display again after a reboot.

`XR_OPENVR_LAUNCH_MODE=none` only registers Direct Mode settings and leaves the
display unchanged. `XR_OPENVR_SET_DISPLAY=0` skips automatic display preparation
when it is already handled elsewhere. Wayland keeps its native launch
environment and requires the desktop compositor's DRM leasing support; physical
output selection through XWayland is unavailable.

If preparation, registration, or execution of Steam fails, the launcher attempts
to restore the selected output's original desktop state. It does not monitor
SteamVR after Steam starts. To restore a display manually after closing SteamVR:

```bash
xrandr --output DP-2 --set non-desktop 0
xrandr --output DP-2 --auto
```

Replace `DP-2` with the selected connector. The existing
`run_openvr_dgpu_direct.sh` remains available for its established dGPU setup.

## Window placement and Wayland

If the stereo window opens on the wrong display, install the X11 tools and run:

```bash
sudo apt install xdotool x11-xserver-utils
./move_openvr_window.sh
```

Select the headset display once and keep the helper running. It moves the window
again after a SteamVR restart or a display position change. Use `--choose` to pick
another display. `XR_OPENVR_WINDOW_OUTPUT` optionally selects a display by name.

In `extended_sbs`, a game's desktop mirror can cover the headset's stereo window.
For Half-Life: Alyx, add `-nowindow` in Steam's game launch options to disable that
mirror. This is unnecessary for Direct Mode.

On a Wayland desktop, `run_openvr.sh` starts a fresh Steam process through XWayland
for `extended_sbs`. This avoids the native Wayland window-fallback assertion seen
in SteamVR 2.18.4. It requires XWayland (`DISPLAY` must be set), and the desktop
itself remains on Wayland. Direct Mode keeps its original launch environment.

`XR_OPENVR_LAUNCH_MODE=none` only registers settings; it cannot persist environment
variables for a future Steam launch. After registration, fully exit Steam and
use this command whenever starting Steam manually on Wayland:

```bash
env -u WAYLAND_DISPLAY XDG_SESSION_TYPE=x11 SDL_VIDEODRIVER=x11 steam
```

XWayland startup and window placement have worked on Ubuntu 26.04 with XREAL Ultra,
but GNOME Wayland/NVIDIA has also shown frame stalls and compositor watchdog aborts.
This remains experimental; compare with a native X11 session if frames stall.
Other Wayland compositors may block window positioning.

Enable the headset display and configure its resolution and refresh rate in your
desktop settings. `xrandr` queries XWayland coordinates here; it does not configure
physical Wayland outputs.

For **Direct Mode**, Wayland support is a separate question: SteamVR needs the
compositor/driver's DRM leasing support. See Valve's
[Linux requirements](https://help.steampowered.com/en/faqs/view/18A4-1E10-8A94-3DDA).
Do not interpret the experimental windowed option as a promise of Direct Mode
support on every Wayland desktop.
