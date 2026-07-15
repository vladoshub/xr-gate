#!/usr/bin/env bash
set -euo pipefail

log() { echo "[install_xreal_ultra_device_access] $*" >&2; }
warn() { echo "[install_xreal_ultra_device_access][WARN] $*" >&2; }
fatal() { echo "[install_xreal_ultra_device_access][ERROR] $*" >&2; exit 1; }

INSTALL_GROUPS="${XR_RUNTIME_INSTALL_GROUPS:-1}"
INSTALL_UDEV="${XR_RUNTIME_INSTALL_UDEV:-1}"

if [[ "$INSTALL_GROUPS" != "1" && "$INSTALL_UDEV" != "1" ]]; then
  log "device access changes disabled"
  exit 0
fi

SUDO=()
if [[ "${EUID:-$(id -u)}" -ne 0 ]]; then
  command -v sudo >/dev/null 2>&1 || fatal "sudo is required when not running as root"
  SUDO=(sudo)
fi

if [[ "$INSTALL_GROUPS" == "1" ]]; then
  TARGET_USER="${SUDO_USER:-${USER:-}}"
  if [[ -n "$TARGET_USER" && "$TARGET_USER" != "root" ]]; then
    log "add user '$TARGET_USER' to video,input,plugdev"
    "${SUDO[@]}" usermod -aG video,input,plugdev "$TARGET_USER" || true
  else
    warn "cannot determine non-root target user for group setup"
  fi
fi

if [[ "$INSTALL_UDEV" == "1" ]]; then
  UDEV_RULE_PATH="${UDEV_RULE_PATH:-/etc/udev/rules.d/70-xreal-ultra.rules}"
  log "install XREAL udev rules: $UDEV_RULE_PATH"
  "${SUDO[@]}" tee "$UDEV_RULE_PATH" >/dev/null <<'UDEV'
# XREAL Air 2 Ultra runtime access for camera/HID paths.
SUBSYSTEM=="usb", ATTR{idVendor}=="3318", ATTR{idProduct}=="0426", MODE="0660", GROUP="plugdev", TAG+="uaccess"
SUBSYSTEM=="hidraw", ATTRS{idVendor}=="3318", ATTRS{idProduct}=="0426", MODE="0660", GROUP="plugdev", TAG+="uaccess"
SUBSYSTEM=="video4linux", ATTRS{idVendor}=="3318", ATTRS{idProduct}=="0426", MODE="0660", GROUP="video", TAG+="uaccess"
UDEV
  "${SUDO[@]}" udevadm control --reload-rules || true
  "${SUDO[@]}" udevadm trigger || true
fi
