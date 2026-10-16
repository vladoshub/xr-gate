# XR Gate SteamVR driver on Windows

[Русская инструкция](WINDOWS.ru.md) · English

This package is a Windows x64 SteamVR driver. XR Gate capture/tracking runs in
Linux, WSL or a Docker container; SteamVR and the game run in Windows. The driver
connects to the runtime's `xr_gate_app_v1` TCP server at **127.0.0.1:45810**.
Display output reaches XREAL through the Windows GPU/video connection. USB/IP
for the cameras/HID does not carry DisplayPort video.

## 1. Download the Windows package

After the accompanying patch has been applied and GitHub Actions has completed:

1. Open the repository's **Actions → XR Gate split build** and the successful run
   for the patched commit.
2. Download artifact **xr-gate-steamvr-windows-x64**. The Linux v2.3.0 runtime ZIP
   does not contain this new Windows driver.
3. Extract the artifact, then extract **xr-gate-steamvr-windows-x64-60Hz-direct.zip**
   into a permanent folder, for example `C:\XR-Gate-SteamVR\60Hz-direct`.
4. The inner **xr_tracking** folder must contain `driver.vrdrivermanifest`,
   `bin\win64\driver_xr_tracking.dll`, `resources`, and the BAT files.

The job also builds 90 Hz and `extended_sbs` variants. Start with 60 Hz. Register
only one XR Gate package at a time. A published release from this workflow also
contains the four Windows ZIPs and SHA-256 files.

## 2. Install the SteamVR driver

Install Steam and SteamVR through Steam. Start SteamVR once, then close it.
Inside the extracted **xr_tracking** folder, double-click **Install-Driver.bat**.
Administrator rights are normally unnecessary. The script locates SteamVR from
its registered runtime path or Steam installation; otherwise it asks for the
SteamVR folder, for example `D:\SteamLibrary\steamapps\common\SteamVR`.

It runs `vrpathreg.exe adddriver` and shows registered paths. Keep the driver
folder in place after registration. If SteamVR blocks the add-on, open
**Settings → Startup/Shutdown → Manage Add-ons** and enable XR Gate.

For an explicit path in PowerShell:

```powershell
.\Manage-Driver.ps1 -Action Install -SteamVR 'D:\SteamLibrary\steamapps\common\SteamVR'
```

To remove or switch packages: close SteamVR, run **Uninstall-Driver.bat** in the
old package, then install the new package. This removes only that registered
driver path, not your SteamVR installation or other drivers.

## 3. Prepare XREAL SBS and the video connection

These are separate operations: **SBS/3D mode** changes the glasses' display
layout; **SteamVR Direct Mode** lets the compositor own a supported display.

1. Connect the glasses to a video-capable USB-C output, or an appropriate
   powered DisplayPort/HDMI-to-USB-C adapter connected to the intended GPU.
2. Before forwarding XREAL USB to WSL, enable full SBS/3D. On XREAL Air 2 Ultra,
   hold **Brightness Up (+) for about 3 seconds** until the beep, as described
   in the root XR Gate README.
3. In Windows Display settings, verify the glasses show the expected stereo
   resolution, normally **3840×1080**, and select **60 Hz** for the 60 Hz package.
   This temporary desktop check happens before Direct Mode takes ownership.
4. Forward cameras/HID and the optional Bluetooth dongle to WSL, then run XR Gate.
   Keep its `xr_client` console open. When offered a display-mode choice in WSL,
   use the current display mode if Windows/the hardware button already set it.

Once USB is attached to WSL, a Windows HID utility cannot control that same USB
device. Use the glasses' hardware button or the Linux-side helper; stop XR Gate
and detach USB before using a Windows HID helper. A USB mode switch/reconnect
may require attaching USB again and restarting the container/runtime.

## 4. Enable Direct Mode in SteamVR

1. With XR Gate running, run **Test-TCP.bat**. It must report `xr_gate_app_v1`.
   Transport success alone does not establish valid tracking: check the startup
   gate and `pose_valid` as well.
2. Start SteamVR. Open **Settings → Developer → Enable Direct Mode**, following
   Valve's instructions. Depending on SteamVR version, enable Advanced Settings
   or open Developer Settings from the SteamVR menu to find this control.
3. Restart SteamVR if requested. Verify VR content is shown in the glasses and
   head/controller tracking responds. Start the game only after this check.

**Direct-Mode-Guide.bat** opens this guide and starts SteamVR. The Direct Mode
toggle remains a manual step: this package does not use an undocumented GPU
command or claim that changing `directMode: true` can force any monitor into
Direct Mode.

Direct Mode support for the particular XREAL EDID, GPU and adapter must be
tested on the real machine. The current driver provides `IVRDisplayComponent`;
it does not add a new Windows GPU/display driver. A successful DLL build or TCP
test is not proof that Windows can acquire this display in Direct Mode.

If the display stays black or Direct Mode is unavailable: check the GPU video
path, SBS resolution/frequency and SteamVR logs. As an **experimental** alternative,
unregister the direct package and install the matching `60Hz-extended_sbs`
package. It uses an ordinary desktop display; its window coordinates may need
adjustment in `resources/settings/default.vrsettings`. This fallback also needs
hardware testing. To return the display to normal use, stop SteamVR, use its
Disable Direct Mode control if available, detach USB from WSL, and switch the
glasses back to 2D with their hardware control.

## TCP settings and daily startup

The Windows package sets `runtimePoseTransport`, `runtimeHandTransport`, and
`runtimeControllerStateTransport` to `tcp`. Edit the `xr_tracking` section of
`resources/settings/default.vrsettings` while SteamVR is stopped:

```json
"runtimeTcpHost": "127.0.0.1",
"runtimeTcpPort": 45810,
"runtimeTcpReconnectMs": 500,
"runtimeTcpTimeoutMs": 1000
```

Host accepts an IPv4 literal or `localhost`. Pose/hand/controller max-age settings
remain available (normally 250 ms). One background TCP connection serves the
head and both hands. On disconnect the cached streams are cleared; quiet or
replayed frames expire. TCP does not use the historical controller input hold.
Linux `auto` remains POSIX SHM; Windows `auto` selects TCP. Explicit Windows UDP
is still selectable, but this guide and the WSL bundle use TCP.

Daily order: **SBS/video → USB into WSL → XR Gate → TCP test → SteamVR → game**.
Docker publishes `127.0.0.1:45810` on its Linux host; WSL's localhost forwarding
then exposes it to Windows. Do not start the direct WSL runtime and Docker
runtime on the same port simultaneously. For a 90 Hz setup, choose the matching
package and optionally set the runtime TCP output rate to 90; this is separate
from the glasses' display refresh rate.

## Build from source / CI

Use MSVC x64, CMake/Ninja, Python 3.10+, OpenVR SDK commit
`0924064316de3effbcd1acf1e309182a2deb1c05`, and nlohmann/json 3.11+ (CI pins 3.11.3).
From an x64 Developer PowerShell in the repository root:

```powershell
.\drivers\openvr_driver\scripts\windows\build_driver.ps1 `
  -OpenVrSdkRoot C:\src\openvr -NlohmannJsonInclude C:\src\json\single_include `
  -Device xreal_ultra -BuildType Release -DisplayFrequency 60 -DisplayMode direct -BuildTests
ctest --test-dir build\drivers\openvr_driver\windows_Release -C Release --output-on-failure
```

Output: `build\drivers\openvr_driver\windows_Release\xr_tracking`. The separate
Windows CI job runs the same tests and packages 60/90 Hz direct/extended variants.
No hardware is available on hosted CI: actual SteamVR loading, GPU Direct Mode,
XREAL USB and Bluetooth need on-device validation.

Sources: [Valve Direct Mode help](https://help.steampowered.com/en/wizard/HelpWithGameIssue/?appid=250820&issueid=370&nodeid=52),
[XR Gate root README](https://github.com/vladoshub/xr-gate#quick-start),
[Microsoft WSL networking](https://learn.microsoft.com/en-us/windows/wsl/networking).
