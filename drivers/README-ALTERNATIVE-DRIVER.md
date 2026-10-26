# XR Gate


# SteamVR on Linux Direct Mode (XREAL Ultra example) | Alternative

**Workaround for systems without built-in x11 (Ubuntu 26.04)**

### 1. Install LightDM + Xfce


```bash
sudo apt update
sudo apt install -y software-properties-common
sudo add-apt-repository -y universe
sudo apt update

sudo apt install -y \
  lightdm lightdm-gtk-greeter \
  xfce4 xfce4-terminal dbus-x11 \
  xserver-xorg xserver-xorg-input-libinput \
  xserver-xorg-video-amdgpu
```

### 1a. Additional for NVIDIA GPU
```bash
bash <<'BASH'
set -e

LIGHTDM_NATIVE_ARCH="$(dpkg --print-architecture)"

read -r LIGHTDM_NV_GL LIGHTDM_NV_VERSION < <(
  dpkg-query -W \
    -f='${Package} ${Architecture} ${Version} ${db:Status-Status}\n' \
    'libnvidia-gl*' 2>/dev/null |
  awk -v arch="$LIGHTDM_NATIVE_ARCH" \
    '$2 == arch && $4 == "installed" {print $1, $3; exit}'
)

LIGHTDM_NV_XORG="xserver-xorg-video-nvidia${LIGHTDM_NV_GL#libnvidia-gl}"

sudo apt install -y "${LIGHTDM_NV_XORG}=${LIGHTDM_NV_VERSION}"

test -f /usr/lib/xorg/modules/drivers/nvidia_drv.so
BASH
```

### 2. Enable lightdm

```bash
sudo mkdir -p /etc/lightdm/lightdm.conf.d

sudo tee /etc/lightdm/lightdm.conf.d/90-xfce.conf >/dev/null <<'EOF'
[Seat:*]
greeter-session=lightdm-gtk-greeter
user-session=xfce
EOF

sudo dpkg-reconfigure lightdm
```

Choose lightdm and:


```bash
sudo systemctl enable lightdm.service --force
sudo systemctl set-default graphical.target
sudo reboot
```


### 2a. Additional install blueman for convenience and Bluetooth control


```bash
sudo apt update
sudo apt install -y bluez blueman rfkill

sudo systemctl enable --now bluetooth.service
sudo rfkill unblock bluetooth
bluetoothctl power on
```




### Run xr_client

```bash
cd ~/xr-gate-release/xr-gate
./run_xr_client.sh --config xreal_ultra
```

## Use SteamVR

###  Register OpenVR driver (once)

For direct USB4/iGPU path, tested with HX370 iGPU:

90HZ
```bash
cd ~/xr-gate-release/xr-gate
XR_TARGET_DEVICE=xreal_ultra \
XR_DISPLAY_FREQUENCY_HZ=90 \
XR_OPENVR_DISPLAY_FREQUENCY_HZ=90 \
XR_OPENVR_DISPLAY_MODE=direct \
./devices/common/linux/scripts/openvr_driver/register_driver.sh
```

60HZ
```bash
cd ~/xr-gate-release/xr-gate
XR_TARGET_DEVICE=xreal_ultra \
XR_DISPLAY_FREQUENCY_HZ=60 \
XR_OPENVR_DISPLAY_FREQUENCY_HZ=60 \
XR_OPENVR_DISPLAY_MODE=direct \
./devices/common/linux/scripts/openvr_driver/register_driver.sh
```

Custom frequency 75HZ:
```bash
cd ~/xr-gate-release/xr-gate
XR_TARGET_DEVICE=xreal_ultra \
XR_DISPLAY_FREQUENCY_HZ=75 \
XR_OPENVR_DISPLAY_FREQUENCY_HZ=75 \
XR_OPENVR_DRIVER_DIR_NAME=openvr_driver_60HZ \
XR_OPENVR_DISPLAY_MODE=direct \
./devices/common/linux/scripts/openvr_driver/register_driver.sh
```

For NVIDIA dGPU with HDMI/DisplayPort -> Type-C DP adapter, 60 Hz is the safer default:

```bash
cd ~/xr-gate-release/xr-gate
XR_OPENVR_REGISTER_METHOD=manual \
XR_OPENVR_RUNTIME_MODE=steamvr \
XR_TARGET_DEVICE=xreal_ultra \
XR_DISPLAY_FREQUENCY_HZ=60 \
XR_OPENVR_DISPLAY_FREQUENCY_HZ=60 \
XR_OPENVR_DISPLAY_MODE=direct \
./devices/common/linux/scripts/openvr_driver/register_driver.sh
```

For NVIDIA dGPU with HDMI/DisplayPort -> Type-C DP adapter, custom frequency 75HZ:

```bash
cd ~/xr-gate-release/xr-gate
XR_OPENVR_REGISTER_METHOD=manual \
XR_OPENVR_RUNTIME_MODE=steamvr \
XR_TARGET_DEVICE=xreal_ultra \
XR_DISPLAY_FREQUENCY_HZ=75 \
XR_OPENVR_DISPLAY_FREQUENCY_HZ=75 \
XR_OPENVR_DRIVER_DIR_NAME=openvr_driver_60HZ \
XR_OPENVR_DISPLAY_MODE=direct \
./devices/common/linux/scripts/openvr_driver/register_driver.sh
```



**Start xr_client before run SteamVR**

### Start on Integrated GPU with USB-C video output without discrete GPU in system

Tested on HX370


Just start Steam and start SteamVR. After you can start SteamVR apps/games.


### Start on Discrete GPU

Tested on RTX6000 Pro Blackwell


In most cases, you will need an adapter to run the glasses with a dGPU.

Check displays:

```bash
xrandr --query | grep " connected"
```

Example output:

```text
DP-6 connected primary 7680x2160+0+1080
DP-4 connected 3840x1080+1920+0
```

Here:

```text
DP-4 = glasses (3840x1080)
DP-6 = main monitor
```

Run direct mode (tested on Nvidia) 60HZ:
```bash
cd ~/xr-gate-release/xr-gate
XR_OPENVR_DGPU_VENDOR=nvidia \
XR_OPENVR_DGPU_OUTPUT=DP-4 \
XR_OPENVR_LAUNCH_MODE=steam \
XR_OPENVR_CLEAR_LOGS=1 \
XR_DISPLAY_FREQUENCY_HZ=60 \
./run_openvr_dgpu_direct.sh 60
```


Run direct mode (tested on Nvidia) custom frequency 75HZ:
```bash
cd ~/xr-gate-release/xr-gate
XR_TARGET_DEVICE=xreal_ultra \
XR_OPENVR_DGPU_VENDOR=nvidia \
XR_OPENVR_DGPU_OUTPUT=DP-4 \
XR_OPENVR_LAUNCH_MODE=steam \
XR_OPENVR_CLEAR_LOGS=1 \
XR_OPENVR_DRIVER_DIR_NAME=openvr_driver_60HZ \
XR_OPENVR_DISPLAY_MODE=direct \
XR_OPENVR_DISPLAY_FREQUENCY_HZ=75 \
XR_DISPLAY_FREQUENCY_HZ=75 \
./run_openvr_dgpu_direct.sh 75
```

Start SteamVR. After you can start SteamVR apps/games.


After finishing the work, I recommend restoring the glasses to their normal state to avoid problems during future launches or if you have an issue with xr_client or the SteamVR driver:

```bash
cd ~/xr-gate-release/xr-gate

XR_STEAMVR_RESTORE_OUTPUT=DP-4 \
XR_STEAMVR_RESTORE_MAIN_OUTPUT=DP-6 \
XR_STEAMVR_RESTORE_LAYOUT=right-of \
./run_openvr_restore_desktop.sh
```


---

# Monado/OpenXR on Linux (XREAL Ultra example) | Alternative


### A. Monado with main monitor — Auto disable main monitor

Example for 90 Hz XREAL Ultra:

```bash
cd ~/xr-gate-release/xr-gate

XR_TARGET_DEVICE=xreal_ultra \
XR_MONADO_DEVICE=xreal_ultra \
XR_DISPLAY_FREQUENCY_HZ=90 \
XR_TRACKING_MONADO_COMPOSITOR_MODE=xcb_fullscreen \
XR_TRACKING_MONADO_XCB_OUTPUT=DisplayPort-1 \
XR_TRACKING_MONADO_XCB_RATE_HZ=90 \
./devices/xreal_ultra/linux/scripts/monado_driver/double_display_fix.sh
```

After stopping Monado driver with `Ctrl+C`, the main display should be restored.

### B. Monado with main monitor — Manually disable main monitor

Check displays:

```bash
xrandr --query | grep " connected"
```

Example output:

```text
DP-6 connected primary 7680x2160+0+1080
DisplayPort-1 connected 3840x1080+1920+0
```

Here:

```text
DisplayPort-1 = glasses
DP-6 = main display
```

Disable main display:

```bash
cd ~/xr-gate-release/xr-gate

XR_TRACKING_MONADO_XCB_OUTPUT=DisplayPort-1 \
XR_TRACKING_MAIN_OUTPUT=DP-6 \
devices/xreal_ultra/linux/scripts/monado_driver/main_display_control.sh off-main
```

Run Monado driver:

```bash
cd ~/xr-gate-release/xr-gate

XR_TARGET_DEVICE=xreal_ultra \
XR_MONADO_DEVICE=xreal_ultra \
XR_DISPLAY_FREQUENCY_HZ=90 \
XR_TRACKING_MONADO_COMPOSITOR_MODE=xcb_fullscreen \
devices/common/linux/scripts/monado_driver/start_monado_driver.sh
```


Enable main display again:

```bash
cd ~/xr-gate-release/xr-gate
XR_TRACKING_MONADO_XCB_OUTPUT=DisplayPort-1 \
XR_TRACKING_MAIN_OUTPUT=DP-6 \
devices/xreal_ultra/linux/scripts/monado_driver/main_display_control.sh on-main
```


### C. Monado without main monitor in system

```bash
cd ~/xr-gate-release/xr-gate

XR_TARGET_DEVICE=xreal_ultra \
XR_MONADO_DEVICE=xreal_ultra \
XR_DISPLAY_FREQUENCY_HZ=90 \
XR_TRACKING_MONADO_COMPOSITOR_MODE=xcb_fullscreen \
devices/common/linux/scripts/monado_driver/start_monado_driver.sh
```
