# XR Gate WPF launcher

Build on Windows with the .NET 10 SDK:

```powershell
.\win\launcher\build.ps1
```

The script tests settings preservation and literal arguments, exercises the real Windows PowerShell process bridge, publishes `out/windows-launcher/XR-Gate.exe` for `win-x64` with `SelfContained`, `PublishSingleFile` and `IncludeNativeLibrariesForSelfExtract`, then smoke-tests the published WPF window. WPF trimming is disabled. No third-party NuGet packages are needed. Users only need the published Windows archive, not the SDK or a .NET installation.

`MainWindow` serializes operations and uses a left/right name picker for Bluetooth pairing and a console for interactive calibration/training. `ProcessRunner` invokes `XrGate-Wsl.ps1` using literal -File arguments for background actions and UTF-16 encoded commands for interactive consoles and receives structured JSON through a per-operation result file. Cancelling a startup wait only cancels the waiting PowerShell process; runtime shutdown goes through the verified Linux runtime identity and its existing SIGTERM cleanup. `WslKeeper` owns a private stdin lease; closing its pipe ends the Linux `cat` process as well as its Windows WSL client.

Setup exit code 20 is a resumable stage, with `restart_windows`, `ubuntu_first_login` or `restart_ubuntu` identifying the required next action. The GUI never reboots Windows automatically. The Ubuntu restart button terminates only the explicitly selected distribution after the user has been told to save work.

Hardware data belongs to `win/hardware/*.env` and `.env`: USB identities, modules, paths, dimensions, optics, controller templates/providers/labels, and `XR_DISPLAY_MATCH`. The executable contains no device VID/PID, headset model or display geometry. Driver frequency matching and its required 60 Hz fallback live in `windows/Driver.ps1`; native display mode changes use the same `DisplayNative.cs` as the CLI.

Diagnostics and status have a 60-second deadline and a Cancel diagnostics button. Cancellation also reaches WSL discovery, and redirected output cannot extend a probe beyond its deadline. Linux diagnostic subcommands have a 10-second deadline and do not abort the remaining checks when one times out. Status reads the current Windows display mode independently of the configured driver frequency. Automatic display selection requires an explicit output, a saved identity or a unique configured label; geometry alone does not identify a headset.

CI entry points:

- `.github/workflows/launcher-tests.yml`: tests, publish, WPF smoke test, portable launcher/scripts artifact.
- `.github/workflows/xr-gate-split-build.yml`: publish plus the complete release archive with SteamVR driver variants.

For a cross-build check on Linux, use `dotnet build win/launcher/XrGate.Launcher.csproj`. `EnableWindowsTargeting` permits compilation, but only Windows can execute the WPF smoke test and validate USB/SteamVR interaction.

The smoke test also invokes real asynchronous Doctor/Test actions with an in-memory backend, clicks the routed Cancel button, verifies that another action can run afterward, and checks that recovery keeps repair controls available while blocking backend actions. The fixture is restricted to `--smoke-test` and does not write user settings or invoke PowerShell/WSL.

`LauncherSettings` distinguishes inherited fields from explicit overrides, reloads a selected hardware preset and notifies WPF bindings. `SettingsRecovery` returns a repairable problem instead of closing the window on a broken env/state file; reset moves the exact original to a unique backup. Operation results require a complete JSON contract, including boolean installed/running fields for successful Status.

SteamVR discovery skips unavailable locations recorded by OpenVR and Steam libraries. An invalid explicit SteamVR folder reports which setting to correct. Install can finish the Linux setup with a deferred-registration warning; use Register SteamVR driver after correcting the folder. Background PowerShell output is plain UTF-8 text, including host, warning and error messages.

Bluetooth discovery and assignment use `override_controller/scripts/linux/bluetooth_controllers.py`, shipped next to the Linux start script and with Windows. The picker accepts one or two distinct MAC addresses and preserves learned mappings for selected sides. Config updates are atomic and follow successful pairing/trust/connect.

The header reads `XR_DEVICE_NAME` from the selected env. A separate read-only DeviceStatus action polls USB presence/attachment without starting WSL, changing hardware selection or writing logs on every poll. CONNECTED means available to Windows, SHARED means attached to WSL, and NOT CONNECTED means absent from the configured selection.

The Linux managed-session supervisor retains the lock until owned services exit, including detached groups. Stop uses a unique inherited session token plus PID start/boot identity and can recover descendants after supervisor failure. Closing also releases Attach-only sessions; incomplete shutdown leaves the window open. USB ownership is retained separately from desired selection so disabling Bluetooth cannot hide an earlier attachment.

The split workflow supports independent Ubuntu/Windows release selection. Windows-only bundles pin a prior published Ubuntu runtime/models release and contain the Bluetooth helper for that older runtime. Package paths use `xr-gate-windows-wsl-usbip`; legacy archive names remain supported for downloads and caches.
