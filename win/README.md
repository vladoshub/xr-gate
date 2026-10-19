# XR Gate: Windows installation with WSL2

[English](START-HERE.html) · [Русский](START-HERE.ru.html)

## 1. Prepare Windows

1. Use Windows 11 x64 with an AVX2-capable CPU.
2. Install Steam and SteamVR. Open SteamVR once, then close it.
3. Extract the entire archive to a permanent folder, such as C:\XR-Gate-WSL. Keep the linux and windows subfolders next to XrGate-Wsl.ps1.
4. Open a regular Windows PowerShell window in that folder. Run the commands below in Windows.

## 2. Install XR Gate

1. Double-click INSTALL.cmd, or run:

~~~powershell
.\INSTALL.cmd
~~~

For Gear VR controllers, use this command instead:

~~~powershell
.\INSTALL.cmd -GearVR
~~~

2. Approve the Windows administrator prompt.
3. If a Windows restart is requested, save your work, restart Windows, and repeat the installation command.
4. If an Ubuntu window opens, create a Linux username and password. At the Ubuntu command prompt, type exit. Run the installation command again.
5. If the installer asks you to restart Ubuntu after enabling systemd, save any work in Ubuntu and run:

~~~powershell
wsl --terminate Ubuntu-24.04
.\INSTALL.cmd
~~~

Repeat your installation options, such as -GearVR, if used. Exit code 20 means you need to complete the displayed step and repeat installation.

6. If driver registration was deferred, close SteamVR and run:

~~~powershell
.\DRIVER.cmd
~~~

If SteamVR is not found, specify its installation folder:

~~~powershell
.\DRIVER.cmd -SteamVR 'D:\SteamLibrary\steamapps\common\SteamVR'
~~~

## 3. Prepare the glasses (using XREAL ULTRA glasses as an example)

1. Connect the XREAL Air 2 Ultra video input to your Windows GPU using a video-capable USB-C port or a powered DP/HDMI-to-USB-C adapter.
2. Put on the glasses or activate their proximity sensor.
3. Hold Brightness Up (+) for approximately three seconds until the beep to enable full SBS.
4. In Windows Settings → System → Display, select Extend these displays. Select the glasses and set 3840×1080 at 60 Hz.
5. Click Identify and note which display number appears in the glasses. If the glasses are missing from Windows, open SteamVR Developer Settings, select Disable Direct Mode, then close SteamVR and repeat step 4.
6. With SteamVR closed, double-click DISPLAY.cmd or run:

~~~powershell
.\DISPLAY.cmd
~~~

7. Select the list row for the glasses. Match the DISPLAY number to Windows Identify; use the displayed name to distinguish monitors. The command saves the selection and configures extended SBS output.
8. If using Gear VR controllers, complete section 5 before continuing.

## 4. Start XR Gate and SteamVR

1. Connect the glasses and enable SBS. Place IMU controllers on a flat surface and keep them still during initial calibration.
2. Double-click RUN.cmd, or run:

~~~powershell
.\RUN.cmd
~~~

3. In the xr_client console, complete the startup prompts and calibration. Keep the cameras uncovered and the room well lit. Move your head slowly for a few seconds when tracking starts.
4. Wait for SteamVR to open.
5. Keep SteamVR Direct Mode disabled for this setup.
6. If XR Gate is blocked, open Settings → Startup/Shutdown → Manage Add-ons and enable it.
7. Check that the SteamVR scene appears in the glasses and responds to head movement, then launch your game.

For later starts, enable SBS and run RUN.cmd. It reapplies the saved display selection using its current desktop coordinates. After changing a cable/port or if the saved display is missing, close SteamVR and run DISPLAY.cmd again.

To start only xr_client, run:

~~~powershell
.\CONSOLE.cmd
~~~

If setup takes longer than 180 seconds, finish the prompts in the open xr_client window and run RUN.cmd again. To allow more time:

~~~powershell
.\RUN.cmd -TimeoutSeconds 300
~~~

If the glasses need to be selected manually:

~~~powershell
usbipd list
.\USB.cmd -XrealBusId 1-2
~~~

Replace 1-2 with the glasses' BUSID from usbipd list.

## 5. Set up Gear VR controllers

1. Connect the glasses and a dedicated USB Bluetooth dongle. If Gear VR was not enabled during installation, run:

~~~powershell
.\INSTALL.cmd -GearVR
~~~

2. Find the dongle's BUSID:

~~~powershell
usbipd list
~~~

3. Attach the dongle:

~~~powershell
.\USB.cmd -BluetoothBusId 1-4
~~~

Replace 1-4 with your dongle's BUSID.

4. Open the Bluetooth pairing console:

~~~powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\XrGate-Wsl.ps1 -Action Pair
~~~

5. Put the controllers into pairing mode and enter the following commands in bluetoothctl. Replace MAC_LEFT and MAC_RIGHT with the actual controller addresses:

~~~text
power on
agent on
default-agent
scan on
pair MAC_LEFT
trust MAC_LEFT
connect MAC_LEFT
pair MAC_RIGHT
trust MAC_RIGHT
connect MAC_RIGHT
scan off
quit
~~~

6. Start the controller registration wizard:

~~~powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\XrGate-Wsl.ps1 -Action Controllers -GearVR
~~~

7. Follow the prompts to assign the left and right controllers and save the configuration.
8. Run RUN.cmd.

If you already have a gearvr.json configuration, import it instead of repeating controller registration:

~~~powershell
.\INSTALL.cmd -GearVR -ControllerConfig 'C:\XR-Gate-WSL\gearvr.json'
~~~

Complete Bluetooth pairing before starting XR Gate. To select a different USB Bluetooth dongle later, repeat step 3 with its BUSID.

## 6. Stop XR Gate

1. Close SteamVR.
2. Press Ctrl+C in the xr_client console and wait for it to stop.
3. Return the selected USB devices to Windows:

~~~powershell
.\DETACH.cmd
~~~

4. To use the glasses as a normal desktop display, switch them back to 2D with their hardware button.

## 7. Change settings or update

### Update the setup scripts

1. Close SteamVR.
2. Extract the new archive into the same setup folder and replace its files, including the linux and windows subfolders.
3. Enable SBS and select the matching resolution and refresh rate in Windows.
4. Run DISPLAY.cmd and select the glasses' screen once.
5. Run RUN.cmd.

### Update XR Gate

1. Close SteamVR and stop xr_client.
2. Run INSTALL.cmd again.
3. Enable SBS, select the matching refresh rate in Windows, then run RUN.cmd. The saved screen selection is applied to the new driver. If the display is no longer found, close SteamVR and run DISPLAY.cmd again.

To install a specific release:

~~~powershell
.\INSTALL.cmd -ReleaseTag v2.8.0
~~~

### Switch to 90 Hz

1. Close SteamVR and stop xr_client.
2. Install the matching driver:

~~~powershell
.\INSTALL.cmd -FrequencyHz 90 -DisplayMode extended_sbs
~~~

3. Set the glasses to SBS/90 Hz and select 90 Hz in Windows display settings.
4. Run DISPLAY.cmd, then RUN.cmd.

### Use an extended SBS display

1. If Direct Mode was enabled, open SteamVR Developer Settings and select Disable Direct Mode. Close SteamVR; xr_client can stay open.
2. Select Extend these displays and 3840×1080 at the installed driver's refresh rate in Windows.
3. Run:

~~~powershell
.\DISPLAY.cmd
~~~

4. Select the glasses' display from the list, then run RUN.cmd.

### Use Direct Mode

1. Close SteamVR and stop xr_client.
2. Install the Direct Mode package:

~~~powershell
.\INSTALL.cmd -FrequencyHz 60 -DisplayMode direct
~~~

3. Start XR Gate and SteamVR, select Enable Direct Mode in SteamVR Developer Settings, and restart SteamVR.
4. If SteamVR shows error 422 or continues to show the Windows desktop in the glasses, follow Use an extended SBS display above.

### Use a custom xr_client profile

1. Place your profile at ~/xr-gate-wsl/config/xr_client.json inside Ubuntu.
2. Stop xr_client and run RUN.cmd again.

## 8. Resolve startup problems

### SteamVR error 422 or Windows desktop in the glasses

1. Open SteamVR Developer Settings and select Disable Direct Mode. Close SteamVR; keep xr_client open.
2. Enable SBS. In Windows Display settings, select Extend these displays and 3840×1080 at the installed driver's refresh rate.
3. Click Identify, then run:

~~~powershell
.\DISPLAY.cmd
.\RUN.cmd
~~~

4. Choose the glasses' display when prompted. Wait for SteamVR to open and check head movement in its scene.
5. If error 422 returns, close SteamVR, repeat DISPLAY.cmd to check the selected screen, and inspect the latest compositor log:

~~~powershell
$vrPaths = Get-Content "$env:LOCALAPPDATA\openvr\openvrpaths.vrpath" -Raw | ConvertFrom-Json
$steamFolder = Split-Path -Parent $vrPaths.config[0]
Get-Content (Join-Path $steamFolder 'logs\vrcompositor.txt') -Tail 100
~~~

### View diagnostics

1. Run:

~~~powershell
.\DOCTOR.cmd
~~~

2. With xr_client running, check the tracking connection:

~~~powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\XrGate-Wsl.ps1 -Action Test -TimeoutSeconds 30
~~~

### Reconnect cameras or Bluetooth

1. Close SteamVR and stop xr_client.
2. Reconnect the USB cable or dongle.
3. Run USB.cmd, then RUN.cmd.
4. If the device is still missing, run:

~~~powershell
wsl -d Ubuntu-24.04 -- lsusb
wsl -d Ubuntu-24.04 -- uname -r
wsl -d Ubuntu-24.04 -u root -- modprobe uvcvideo
wsl -d Ubuntu-24.04 -u root -- modprobe btusb
wsl -d Ubuntu-24.04 -- bluetoothctl list
wsl -d Ubuntu-24.04 -u root -- dmesg
~~~

5. If a module is missing, update WSL:

~~~powershell
wsl --update --web-download
~~~

6. Save your work in Ubuntu, restart the distribution, and reconnect the devices:

~~~powershell
wsl --terminate Ubuntu-24.04
.\USB.cmd
.\RUN.cmd
~~~

If a custom kernel is configured in %USERPROFILE%\.wslconfig, remove or update that kernel setting before repeating these steps.

### Restore the Windows tracking connection

1. Keep xr_client running and run:

~~~powershell
Test-NetConnection 127.0.0.1 -Port 45810
wsl -d Ubuntu-24.04 -- ss -ltnp
~~~

2. For WSL NAT networking, enable localhost forwarding in %USERPROFILE%\.wslconfig. Preserve the file's other settings:

~~~ini
[wsl2]
localhostForwarding=true
~~~

3. Save work in all WSL distributions, then run:

~~~powershell
wsl --shutdown
.\RUN.cmd
~~~

4. If connection still fails, check the VPN/firewall settings and close any other process using port 45810.

### Create a regular Ubuntu user

If Ubuntu starts as root:

1. Open Ubuntu and create a user. Replace xruser with your preferred Linux username:

~~~bash
adduser xruser
usermod -aG sudo xruser
~~~

2. Set the user in /etc/wsl.conf, preserving the file's other sections:

~~~ini
[user]
default=xruser
~~~

3. Save your Ubuntu work, then run from Windows:

~~~powershell
wsl --terminate Ubuntu-24.04
.\INSTALL.cmd
~~~

### Install a missing Windows prerequisite

1. If winget is missing, install or update Microsoft App Installer, then repeat INSTALL.cmd.
2. If Windows reports that virtualization is disabled, enable SVM/AMD-V or VT-x in BIOS/UEFI, restart Windows, and repeat INSTALL.cmd.

