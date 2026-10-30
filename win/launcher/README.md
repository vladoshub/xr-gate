# XR Gate Windows launcher

XR Gate runs through WSL 2. The portable WPF launcher includes .NET; users open `XR-Gate.exe` from the complete Windows archive.

Select the connected glasses display → Install → Start VR. Install asks for confirmation, offers required restarts and remembers unfinished setup. USB connects with Start VR and returns to Windows on Stop or application close. Gestures is the default, with Bluetooth off.

- **Run:** setup, display selection, launch, stop and activity.
- **Controllers:** input mode, Bluetooth pairing and training.
- **Audio:** saved Windows source and Ubuntu playback output, including No audio.
- **Settings:** hardware preset, display and Show command prompt.
- **SBS:** optional `SBS_RESOLUTION=WIDTHxHEIGHT` in the hardware env controls Start VR/F5 readiness using the current Windows display dimensions. A known mismatch appends `(SBS OFF)` to the device label; a matching mode has no suffix. Unknown/disconnected modes block VR without an OFF label. Missing/empty settings disable the extra mode check. PowerShell checks the mode again before VR startup. The Ultra profile additionally requires EDID model `MRG3136` AND EDID name `Air 2 Ultra`; both checks also apply to saved/manual display choices.
- **Protect device display during tracking** (Settings, on by default): while XR Gate is starting or tracking, keeps the mouse pointer and ordinary windows off the selected secondary SBS display. Stop, an observed runtime exit, disabling the checkbox or closing the launcher releases protection. It stays idle while the launcher is simply open. The primary monitor and SteamVR compositor are excluded; other desktop monitors remain usable. Selection uses the exact monitor interface identity, including after reconnects, and never guesses a screen from its number or resolution.
- **Tools:** diagnostics, driver registration and tracking checks.

Build on Windows with the .NET 10 SDK:

```powershell
.\win\launcher\build.ps1
```

CI uses `.github/workflows/launcher-tests.yml` and `.github/workflows/xr-gate-split-build.yml`. The build script runs core tests, publishes a self-contained `win-x64` executable and runs the WPF UI smoke test. Linux supports cross-compilation; the UI and real USB/SteamVR interaction require Windows.

Hardware settings belong in `win/hardware/*.env`; overrides belong in `.env`. Keep the bundled PowerShell and Linux helpers with the executable. See [the user guide](../README.md) and [Russian guide](../README.ru.md).

Display protection does not change display topology, resolution, refresh rate, focus or SteamVR settings, and does not create an overlay over the compositor. Windows moved back to the desktop remain there after Stop. It is a desktop guard, not exclusive display ownership: elevated applications and exclusive fullscreen mode changes can bypass it. If SteamVR still fails when an application enters fullscreen on another monitor, collect `vrcompositor.txt` and `vrserver.txt`; protecting the XR screen does not establish the cause of that failure. Native hook and SteamVR behavior require testing on Windows with the actual displays.


The Controllers list offers **Gestures (controller emulation)** (default) and
**Gestures (grab / pinch)**. Existing `XR_CONTROLLER_MODE=gestures` settings keep
the controller emulation mode; `simple_gestures` uses pinch for Trigger and an
open palm facing the user for Grip. Outside the touchpad, a fist holds Grip +
Trigger, V sends left X / right A, thumbs-up sends left Y / right B, and two index
fingers pointing toward each other send Menu. No calibration is needed.
The **Gestures** button opens the matching tab in the compact guide with one finger
map and gesture tables. **Support VR controllers**
opens the Samsung Gear VR Controller layouts for LEFT and RIGHT and explains IMU-assisted tracking.
Both guides work offline and ship in the Windows package.

Simple mode's virtual touchpad is enabled by default. Fully straighten all five
fingers, separate thumb and index, and point the palm directly away from your
head briefly to capture a head-relative centre. Move within a 10 cm radius,
then show V to click; moving the V pose still changes the panel direction.
Wrist rotation does not exit the panel; the 35-degree limit is for activation only. Loss of hand/head tracking exits. Thumbs-up also exits without pressing B/Y. Release the exit pose before
using it as a normal button. Set `XR_SIMPLE_GESTURES_TOUCHPAD=0`
in `.env` to disable only the panel; restart tracking after changing it.

During a simple-mode touchpad session, pinch sends Trigger, an open palm toward
yourself sends Grip, and V clicks the panel while the axes keep working.
V sends no A/X in this mode, a fist does not click, and thumbs-up exits without pressing B/Y.
For V, point the fingers up and the palm away from you. An angled wrist
releases the V button while keeping the panel active. For the thumb gesture, curl index, middle, ring and pinky into a fist. Keep
the thumb away from the palm and point its main phalanx straight up. The tip
may be straight or bent; a backward arch is not required. The upward angle
limit stays at 15 degrees (20 while holding).


Simple gestures are the default for new sessions/settings. To explicitly select
controller emulation on Linux, use `SIMPLE_GESTURES=0 EMULATION_CONTROLLER=1`.
Existing saved launcher selections are preserved.

Simple-mode settings in `start_xr_runtime_adapter_shm.sh` (or launch environment):

```bash
SIMPLE_GESTURES=1
SIMPLE_GESTURES_TOUCHPAD_RADIUS_M=0.10
SIMPLE_GESTURES_POSE_YAW_OFFSET_DEGREES=30
```

The radius is in metres (0.01–1.0). Positive yaw turns both output hands outward,
over all existing offsets; set it to 0 to disable. The correction affects the
whole skeleton and virtual controller after recognition, so gesture directions
and touchpad axes remain head-relative. As in controller emulation, it is skipped
on a side using controller IMU orientation and returns when that side switches
back to optical hand orientation. It does not accumulate on reconnect.
In the Windows `.env`, use the same radius/yaw names prefixed with `XR_`.
Restart tracking after changing them.
