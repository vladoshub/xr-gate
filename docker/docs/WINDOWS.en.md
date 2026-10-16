# Docker runtime on Windows through WSL

[Русский](WINDOWS.ru.md) · English

Use Windows 11 x64, Ubuntu 24.04 **WSL 2**, a dedicated USB Bluetooth dongle if
needed, and the AVX2 runtime requirements from the Docker README. WSL application
version 3.x still runs WSL 2 distributions. This USB architecture uses **Docker
Engine in Ubuntu**, not Docker Desktop's separate VM.

## Install WSL and Ubuntu first

In **PowerShell as Administrator**, enable WSL and install USB support:

```powershell
wsl --install --no-distribution
wsl --update --web-download
winget install --exact --id dorssel.usbipd-win
```

Restart Windows if requested. In an **ordinary PowerShell** window under the
Windows user who will run XR Gate:

```powershell
wsl --set-default-version 2
wsl --install -d Ubuntu-24.04 --web-download
```

Finish Ubuntu's first-user setup when requested. Choose a lowercase Linux
username and password. **Password characters are not displayed while typing.**
Do not register Ubuntu under a different administrator account. For an existing
same-account Ubuntu installation, `windows/Setup-Windows.ps1` is an optional
elevated WSL/usbipd update helper; it does not automatically elevate itself.

## Install Docker Engine and the container

Extract/download this entire repository-root **docker/** folder into a permanent
Windows location. In ordinary PowerShell, from that folder:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\windows\Install-XrGate.ps1
```

This copies the bundle to the Linux filesystem, prepares systemd/BlueZ,
installs local Docker Engine and builds the release-based image. If it enables
systemd, save Ubuntu work, terminate the distribution with
`wsl --terminate Ubuntu-24.04`, and repeat installation.

Keep the image/runtime files in Linux, not `/mnt/c`. The default installed
directory is **~/xr-gate-docker**. Inspect the script's `-LinuxPath` parameter
if you need another location.

Before invoking helpers in any new PowerShell window, use `Set-ExecutionPolicy -Scope Process Bypass` if needed; it applies only to that process.

## USB attachment and run

Connect XREAL. For SteamVR, enable full SBS first with the glasses' hardware
control and verify the Windows stereo display mode. From the extracted Docker
folder's ordinary PowerShell:

```powershell
.\windows\Usb-XrGate.ps1 -Action List
```

Note the actual BUSIDs for XREAL **3318:0426** and your optional Bluetooth dongle.
Once per device, in elevated PowerShell from the same folder:

```powershell
.\windows\Usb-XrGate.ps1 -Action Bind -BusId 1-2,1-4
```

The BUSIDs above are examples. In ordinary PowerShell, attach your selections:

```powershell
.\windows\Usb-XrGate.ps1 -Action Attach -BusId 1-2,1-4
```

The devices are unavailable to Windows while attached. Leave an Ubuntu shell
open so the VM remains running.

Start the runtime:

```powershell
.\windows\XrGate.ps1 -Command Run
```

For host BlueZ/D-Bus access:

```powershell
.\windows\XrGate.ps1 -Command Run -Bluetooth
```

Keep the interactive `xr_client` console open and complete its startup gate.
Test from another Windows PowerShell:

```powershell
.\windows\Test-XrGate.ps1
```

Windows should receive frames at **127.0.0.1:45810** through Docker's host
localhost publication and WSL localhost forwarding. TCP transport success does
not establish valid tracking. Use the separately built Windows SteamVR driver
only after this succeeds.

## Pairing and overrides

Open Ubuntu from Start or `wsl -d Ubuntu-24.04`. Start host Bluetooth:

```bash
sudo systemctl enable --now bluetooth
bluetoothctl
```

Follow the Gear VR / VR-PARK steps in **CONTROLLERS.en.md**. Bluetooth pairing
lives in this Ubuntu's BlueZ database. Configure the Linux copy of `.env` and
`config/`; use Explorer through `\\wsl.localhost\Ubuntu-24.04\home`, then your
Linux username and `xr-gate-docker`.

## Stop, return USB, troubleshoot

Stop with Ctrl+C or:

```powershell
.\windows\XrGate.ps1 -Command Stop
```

Use `Usb-XrGate.ps1 -Action Detach -BusId 1-2,1-4` with your real BUSIDs, or
`usbipd detach --busid <BUSID>` after stopping XR Gate. After sleep, reboot
or reconnection, attach USB again and recreate the container.

Run `XrGate.ps1 -Command Doctor` and check the USB list, `btusb`/UVC/HID nodes,
BlueZ, firmware and the Ubuntu kernel. Update WSL if required; an old custom
kernel can lack necessary modules. Keep a separate Windows Bluetooth adapter
for normal input devices. DisplayPort video stays on the Windows GPU path;
USB/IP does not forward it.

For the easiest first installation without Docker, use the separate beginner
WSL installer archive supplied alongside this patch. Both methods expose the
same TCP endpoint; run one runtime method at a time.

Sources: [WSL install](https://learn.microsoft.com/en-us/windows/wsl/install),
[USB attachment](https://learn.microsoft.com/en-us/windows/wsl/connect-usb),
[WSL networking](https://learn.microsoft.com/en-us/windows/wsl/networking).
