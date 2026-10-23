# XR Gate WPF launcher

Build on Windows with the .NET 10 SDK:

```powershell
.\win\launcher\build.ps1
```

The script tests settings preservation and literal arguments, exercises the real Windows PowerShell process bridge, publishes `out/windows-launcher/XR-Gate.exe` for `win-x64` with `SelfContained`, `PublishSingleFile` and `IncludeNativeLibrariesForSelfExtract`, then smoke-tests the published WPF window. WPF trimming is disabled. The WASAPI bridge uses pinned `NAudio.Wasapi` 2.2.1 and its core dependency; their MIT notice is included under `licenses`. Users only need the published Windows archive, not the SDK or a .NET installation.

`MainWindow` serializes operations. Tabs are Run, Controllers, Audio, Settings and Tools. Run contains USB attachment and activity; Controllers contains the mode and left/right Bluetooth picker; Audio contains WASAPI routing; Tools contains Diagnostics, driver registration and tracking checks. Interactive calibration/training uses a separate console. `ProcessRunner` invokes `XrGate-Wsl.ps1` using literal -File arguments for background actions and UTF-16 encoded commands for interactive consoles and receives structured JSON through a per-operation result file. Cancelling a startup wait only cancels the waiting PowerShell process; runtime shutdown goes through the verified Linux runtime identity and its existing SIGTERM cleanup. `WslKeeper` owns a private stdin lease; closing its pipe ends the Linux `cat` process as well as its Windows WSL client.

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

The centered header reads `XR_DEVICE_NAME` from the selected env. A separate read-only DeviceStatus action polls USB presence/attachment without starting WSL, changing hardware selection or writing logs on every poll. The header shows CONNECTED in white when available to Windows, TRACKING in green when attached to WSL, and NOT CONNECTED in gray when absent from the configured selection. The USB observation protocol retains SHARED for compatibility; the launcher displays it as TRACKING.

The Linux managed-session supervisor retains the lock until owned services exit, including detached groups. Stop uses a unique inherited session token plus PID start/boot identity and can recover descendants after supervisor failure. Closing also releases Attach-only sessions; incomplete shutdown leaves the window open. USB ownership is retained separately from desired selection so disabling Bluetooth cannot hide an earlier attachment.

`ControllerMode` persists as `XR_CONTROLLER_MODE=gearvr|gestures`. Gestures removes the physical controller publishers from generated profiles and disables inherited defaults, including for `config/xr_client.json`; user profiles are never overwritten. Gear VR Controllers enables automatic publisher startup, using the preset's provider/template and preserving paired MACs and learned mappings. An empty setting preserves legacy command-line behavior.

The hardware picker enumerates `hardware/*.env` and matches `XR_USB_IDS` against the read-only Windows Configuration Manager inventory, before WSL/usbipd installation if necessary. All configured IDs must be present; multiple matching presets require manual selection. Picking a preset turns off `XR_AUTO_DETECT_HARDWARE`, and an existing explicit preset remains manual on migration. Re-enable detection to resume it. Neither detection nor dropdown population binds or attaches USB.

`XR_SHOW_COMMAND_PROMPT=0` hides the tracking console by default; set it through Settings before Start VR to show the same interactive console as before. Display selection and controller training still use visible consoles. The startup gate, calibration and USB transport are unchanged. Enable the tracking console for manual calibration/control commands.

`windows/RuntimeWindow.ps1` records the exact Windows runtime process and a per-launch cancellation token. Stop first marks a pending start cancelled, then stops the Linux session, closes its owned Windows console, and only then detaches selected USB. A delayed Linux start checks this token while holding the runtime lock. Cleanup uses the session's saved settings, allowing Stop even when `.env` is subsequently damaged or changed. Closing during installation waits for setup to finish before cleanup. CI exercises mode/profile behavior, preset matching, cancellation, console identity and WPF close/cleanup paths.

`Assets/XrGate.ico` supplies both the PE application icon and WPF window icon. The original vector design and optional Pillow regeneration script are beside it; CI consumes the committed multi-size ICO without image dependencies.

The split workflow supports independent Ubuntu/Windows release selection. Windows-only bundles pin a prior published Ubuntu runtime/models release and contain the Bluetooth helper for that older runtime. Package paths use `xr-gate-windows-wsl-usbip`; legacy archive names remain supported for downloads and caches.

## WASAPI playback bridge

The Audio tab exposes persistent Windows source and Ubuntu output identities, including No audio and remembered unavailable devices. Audio choices save separately from other unfinished form fields. Refresh never replaces a missing selection with the first enumerated device. Windows endpoint IDs and a hash of USB VID/PID, serial (or unique model), interface and PCM device survive ordinary replug/card renumbering. Indistinguishable serial-less duplicates are unavailable until ambiguity is resolved.

`AudioBridge` captures a render endpoint through shared WASAPI loopback at 48 kHz, stereo, S16. Windows converts the source mix format; ALSA `plughw` negotiates the target hardware format. A 20 ms capture period, bounded drop-oldest queue, 160 ms packet expiry, per-packet acknowledgement and I/O deadlines limit stale audio. The only transport is the private `wsl.exe` stdin/stdout; it has no network listener or dependency on NAT/mirrored networking. Capture callbacks never wait for playback.

`win/linux/xr_gate_audio.py` enumerates real ALSA playback nodes through sysfs, excluding software/WSLg outputs. The Windows package carries this helper, so an older Ubuntu runtime archive is usable after Install / continue installs `libasound2t64` and adds audio access. The receiver validates bounded headers/packets, opens the selected identity after resolving its current card index, acknowledges accepted packets and drops queued hardware sound on close. EOF ends the Linux worker. Only the owned WSL client is terminated if graceful shutdown times out.

Audio has its own worker and cancellation scope. USB/runtime Start/Stop and window close control that worker; a selected device disappearing causes an independent reconnect loop. The loop follows default-source changes but never falls back from a missing explicit source/output. An audio failure is logged and does not fail VR startup. Standalone PowerShell/.cmd sessions do not own this GUI audio bridge.

The existing CI workflow runs Python protocol/discovery tests, ALSA null-device playback, C# persistence/queue/worker tests and real pipe EOF/forced-stop tests. Published WPF smoke mode also checks connected/disconnected/reconnected picker selections without touching audio hardware or user settings. Actual WASAPI capture and USB/IP playback quality still require Windows/WSL hardware testing.
