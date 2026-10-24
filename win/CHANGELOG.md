## No controllers, global hotkeys and tracking restart

- Add No beside Gestures (still the default) and Gear VR Controllers; disable hand/controller services and pass controller-only invalid-pose mode with no input transport to the adapter.
- Add Settings → Hotkeys: global F5 Start VR, F6 Stop tracking and F7 Restart tracking, with optional disable, conflict reporting and key release on exit.
- Restart only the managed XR Gate runtime while retaining SteamVR, display/driver state and USB attachments; wait for a fresh HMD stream and allow Stop during restart.
- Add concise hover hints for every Run button and update the English/Russian guides.

## Ubuntu account continuation and SteamVR shutdown

- Continue installation after Ubuntu account creation and `exit`, using the detected Linux username and bounded probes; show prominent manual instructions if continuation is unavailable.
- Blink Install only when setup is incomplete and the button is available; preserve genuine reboot requirements.
- Request graceful SteamVR exit on Stop/launcher shutdown and Delete without preventing XR Gate/USB cleanup if SteamVR stays open.
- Document first-login password prompts and troubleshooting in English and Russian; replace machine-specific username examples.

## Firmware cleanup before Ubuntu deletion

- Disable the installed firmware service and release this Ubuntu's firmware mount before unregistering it, including when Bluetooth was later disabled.
- Preserve unrelated mounts and kernel firmware paths; serialize firmware changes across WSL distributions.
- Explain how to recover an existing stale mount after reinstalling Ubuntu and keep encoded installation requests out of firmware failure logs.

## Delete Ubuntu and automatic audio selection

- Add Delete in Run with confirmation naming the saved XR Gate Ubuntu distribution and explaining permanent loss of all its Linux data.
- Stop audio/tracking and release selected USB before unregistering Ubuntu; retain setup state on failure and reset readiness after deletion.
- Add Audio Enable, on by default, with immediate persistence and preserved source/output choices when disabled.
- After Start VR attaches USB, refresh audio outputs and keep an available saved output or select the first available one.

## Background WSL startup

- Leave simple native arguments unquoted so WSL recognizes its distribution, user and exec options.
- Keep quotes and escaping for paths with spaces, empty values and literal quotes.
- Return runtime log tails as plain JSON strings in Windows PowerShell 5.1.

## Controller settings autosave

- Save Controllers fields automatically; text fields wait briefly after typing.
- Keep the last valid settings when an adapter identifier is incomplete, with an inline message.
- Save pending valid edits on close and preserve unrelated settings and preset defaults.

## Runtime exit and camera selection

- Stop waiting for TCP when the owned runtime exits; keep device status live during startup.
- Report unexpected exits and current-session startup errors in Activity Log, including with a hidden console.
- Pass XR_CAMERA_DEVICE to capture and optionally discover the actual capture node using configured USB IDs.
- Wait for Linux device nodes after attachment, exclude V4L2 metadata nodes and require HID for the supplied XREAL preset.

## Guided Windows setup

- Select the connected glasses display before Install; block Start VR until setup, display and device are ready.
- Confirm WSL installation, offer required restarts and resume unfinished setup after reopening.
- Highlight the next setup button and show concise Activity Log instructions.
- Default to Gestures with Bluetooth off; attach USB automatically on Start VR.
- Show controller calibration stages once, including completion, with a hidden tracking console.
- Remove standalone batch wrappers from the GUI package and shorten English/Russian guides.

# Windows Launcher 1.1.0

- Keep expected administrator-fixture failures out of the CI shell's LASTEXITCODE, and keep firmware contract tests independent of real WSL distributions.
- Correct GitHub JSON-array handling in CI and installer runtime selection. Missing release lookup no longer blocks an independent Windows build.
- Enable Bluetooth label, project link and configurable XR_DEVICE_NAME in the header.
- Read-only CONNECTED / NOT CONNECTED / SHARED USB status.
- Named left/right Bluetooth selection with an optional side; automatic pairing, trust, connect and configuration backup.
- Shared Linux Bluetooth helper included beside start_override_controller.sh in Ubuntu and bundled with Windows for older runtime releases.
- Managed runtime/controller supervisors collect detached services; Stop validates ownership and waits before detaching USB.
- Closing after Attach-only use also cleans USB. Partial detach errors retain ownership for retry and do not skip other devices.
- Package renamed to xr-gate-windows-wsl-usbip. Independent Ubuntu/Windows release switches; Windows includes SteamVR drivers.
- Windows-only packages pin an existing Ubuntu runtime/models release. Legacy Windows package names remain readable.

# Windows Launcher 1.0.7

- USB attach/detach can be cancelled, including by closing the window. The desktop USB action has a three-minute deadline; cancellation terminates its transient process tree while Run retains the separate scoped runtime shutdown.
- Every usbipd command, including state, bind, attach and detach, has a configurable 30-second deadline (`XR_USB_COMMAND_TIMEOUT`, 5–120 seconds), also for command-line scripts. A stalled command is stopped and reports the failing arguments. Native stderr remains visible during the wait.
- Re-read selected Windows instance IDs before attach and firmware retry to resolve changed BUSIDs without selecting another device. Partial attachment is reported on cancellation; previously attached devices are not reset automatically.
- CI covers real native-process deadlines and cleanup, USB child-process cancellation, preservation of the separate tracking console, changed BUSIDs, retry, and WPF Cancel/close recovery.

# Windows Launcher 1.0.6

- Bluetooth settings accept an optional single VID:PID (`XR_BLUETOOTH_USB_ID`). With Bluetooth enabled and BUSID empty, Install and Attach resolve the matching adapter at its current USB port. Explicit BUSID and explicit advanced instance settings take priority; saved state is used when no selector is configured.
- The supplied XREAL preset defaults to the tested ASUS USB-BT500 (`0b05:190e`); the generic template leaves the identity empty. Device identities remain configuration values. A configured identity never falls back to an unrelated system adapter.
- Multiple matching adapters require a BUSID unless the saved instance identifies exactly one of the matches. Missing explicit BUSIDs and missing matching adapters fail with selection instructions. Disabled Bluetooth does not attach an adapter.
- CI covers port changes, selection priority, old system-adapter state, ambiguous/missing devices, GUI persistence, profile changes and the real WPF field binding.

# Windows Launcher 1.0.5

- Install prepares the configured Ubuntu Bluetooth firmware packages and enables a persistent `xr-gate-wsl-firmware.service` before Bluetooth starts. Its helper and configuration are copied into Linux, so boot does not depend on the Windows archive being accessible.
- Attach USB restores the firmware mount and kernel search path, including when Bluetooth was enabled after installation. Requested compressed firmware is unpacked from installed Ubuntu packages. Vendor filenames come from Bluetooth kernel requests or configuration.
- If the selected attached dongle has no ready HCI controller, only that dongle is reattached after firmware setup. Newly discovered missing firmware can trigger one additional recovery attempt. Other devices and healthy dongles stay connected.
- Readiness matches the selected USB VID:PID to its exact HCI object in BlueZ. An unrelated working adapter cannot mask a failure. USB identities come from saved device selection; no vendor IDs are built into the helper.
- Regression coverage includes WSL mount/search-path loss, boot setup, compressed firmware, multiple adapters, bounded recovery and disabled Bluetooth. Windows CI runs the PowerShell connection contract tests.

# Windows Launcher 1.0.4

Addresses all ten findings from the Windows 1.0.3 audit:

| Finding | Change | Regression coverage |
| --- | --- | --- |
| F01 | Read driver JSON with a case-sensitive dictionary, including debugMode/DebugMode | Real driver settings in verify-runtime-contracts.ps1 |
| F02 | Preserve the elevated PowerShell exit code and return structured errors | Child-process codes 0, 20 and failure with a quoted Unicode path |
| F03 | Canonicalize the Linux installation root and reject /, equivalent paths and symlinks to / | Linux runtime readiness tests; no privileged writes |
| F04 | Read saved state and Steam/OpenVR UTF-8 files explicitly | Save-State/Load-State round trip without BOM and Unicode paths |
| F05 | Reload inherited hardware values and labels; retain explicit overrides | C# profile changes, save/reload and cached-choice overrides |
| F06 | Enforce one TCP read deadline, cap line size and allow cancelling tracking checks/USB listing | Real partial TCP streams; process cancellation; Windows WPF routed Cancel smoke test |
| F07 | Verify actual OpenVR registration before trusting the saved cache | Missing, restored, existing and normalized registration paths |
| F08 | Require an HCI controller for enabled Bluetooth within a configured deadline | Empty/delayed HCI and disabled Bluetooth fixtures |
| F09 | Reject missing, malformed and inconsistent action results | C# schema tests and a real child process without a result |
| F10 | Open configuration recovery without overwriting the original files | Corrupt state/env, missing preset, exact backup and reload; WPF recovery smoke test |

Controller scan duration and Bluetooth readiness duration are configuration values. Read-only checks and Stop do not save pending GUI edits. USB listing remains available during tracking. Concurrent Stop/close requests share the same shutdown operation.

The release CI runs the installer contract and TCP checks as well as publishing the self-contained executable. WPF smoke mode tests cancellation/re-entry and recovery without touching WSL, SteamVR or USB. Actual installation, display changes and Bluetooth pairing still need validation on Windows hardware.
