# XR Gate Windows launcher

XR Gate runs through WSL 2. The portable WPF launcher includes .NET; users open `XR-Gate.exe` from the complete Windows archive.

Select the connected glasses display → Install → Start VR. Install asks for confirmation, offers required restarts and remembers unfinished setup. USB connects with Start VR and returns to Windows on Stop or application close. Gestures is the default, with Bluetooth off.

- **Run:** setup, display selection, launch, stop and activity.
- **Controllers:** input mode, Bluetooth pairing and training.
- **Audio:** saved Windows source and Ubuntu playback output, including No audio.
- **Settings:** hardware preset, display and Show command prompt.
- **Tools:** diagnostics, driver registration and tracking checks.

Build on Windows with the .NET 10 SDK:

```powershell
.\win\launcher\build.ps1
```

CI uses `.github/workflows/launcher-tests.yml` and `.github/workflows/xr-gate-split-build.yml`. The build script runs core tests, publishes a self-contained `win-x64` executable and runs the WPF UI smoke test. Linux supports cross-compilation; the UI and real USB/SteamVR interaction require Windows.

Hardware settings belong in `win/hardware/*.env`; overrides belong in `.env`. Keep the bundled PowerShell and Linux helpers with the executable. See [the user guide](../README.md) and [Russian guide](../README.ru.md).
