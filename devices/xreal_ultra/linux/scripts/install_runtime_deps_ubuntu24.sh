#!/usr/bin/env bash
set -euo pipefail

log() { echo "[install_runtime_deps_ubuntu24] $*" >&2; }
warn() { echo "[install_runtime_deps_ubuntu24][WARN] $*" >&2; }
fatal() { echo "[install_runtime_deps_ubuntu24][ERROR] $*" >&2; exit 1; }

usage() {
  cat <<'USAGE'
Install Ubuntu 24.04 runtime dependencies for an already built XREAL Ultra
runtime package.

This script installs only system/native dependencies needed to run the packaged
artifact on a clean Ubuntu 24.04 machine. It does not install build toolchains,
-dev packages, legacy Python/GStreamer capture_service dependencies, or project
code through pip.

Usage:
  devices/xreal_ultra/linux/scripts/install_runtime_deps_ubuntu24.sh [options]

Options:
  --no-apt       Do not install apt packages.
  --no-groups    Do not add the current user to video/input/plugdev.
  --no-udev      Do not install XREAL udev rules.
  --no-venv      Do not create/repair package-local thin venv.
  --skip-check   Skip Python import and ELF dependency checks at the end.
  -h, --help     Show this help.

Python policy:
  The package-local venv stays thin and uses --system-site-packages.
  Runtime Python code is limited to xr_client/tools plus capture_client.
  The removed legacy Python capture_service is intentionally not supported here.

Environment:
  XR_PACKAGE_ROOT   Runtime package root. Auto-detected when the script is
                    inside out/xreal_ultra/devices/xreal_ultra/linux/scripts.
  XR_RECREATE_VENV  If 1, recreate bin/python-runtime/venv. Default: 0.
USAGE
}

INSTALL_APT=1
INSTALL_GROUPS=1
INSTALL_UDEV=1
INSTALL_THIN_VENV=1
RUN_CHECKS=1

while [[ $# -gt 0 ]]; do
  case "$1" in
    --no-apt) INSTALL_APT=0 ;;
    --no-groups) INSTALL_GROUPS=0 ;;
    --no-udev) INSTALL_UDEV=0 ;;
    --no-venv) INSTALL_THIN_VENV=0 ;;
    --skip-check) RUN_CHECKS=0 ;;
    -h|--help) usage; exit 0 ;;
    *) fatal "unknown option: $1" ;;
  esac
  shift
done

if [[ "$(uname -s)" != "Linux" ]]; then
  fatal "this installer is Linux-only"
fi

if [[ -r /etc/os-release ]]; then
  # shellcheck source=/dev/null
  source /etc/os-release
  if [[ "${ID:-}" != "ubuntu" || "${VERSION_ID:-}" != "24.04" ]]; then
    warn "tested target is Ubuntu 24.04; detected ID=${ID:-unknown} VERSION_ID=${VERSION_ID:-unknown}"
  fi
else
  warn "/etc/os-release not found; cannot verify Ubuntu version"
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEFAULT_PACKAGE_ROOT="$(cd "$SCRIPT_DIR/../../../.." && pwd)"
XR_PACKAGE_ROOT="${XR_PACKAGE_ROOT:-$DEFAULT_PACKAGE_ROOT}"

# Detect whether this is the deploy package root or a source checkout root.
PACKAGE_MODE=0
if [[ -d "$XR_PACKAGE_ROOT/bin" && -d "$XR_PACKAGE_ROOT/devices/xreal_ultra" ]]; then
  PACKAGE_MODE=1
fi

SUDO=()
if [[ "${EUID:-$(id -u)}" -ne 0 ]]; then
  command -v sudo >/dev/null 2>&1 || fatal "sudo is required when not running as root"
  SUDO=(sudo)
fi

apt_install_required() {
  local packages=("$@")
  [[ ${#packages[@]} -gt 0 ]] || return 0
  log "apt install required runtime packages: ${#packages[@]}"
  DEBIAN_FRONTEND=noninteractive "${SUDO[@]}" apt-get install -y --no-install-recommends "${packages[@]}"
}

if [[ "$INSTALL_APT" == "1" ]]; then
  log "apt update"
  "${SUDO[@]}" apt-get update

  runtime_packages=(
    # Shell/package/runtime helpers used by launch scripts and diagnostics.
    ca-certificates curl wget tar unzip rsync jq procps psmisc lsof file acl

    # Thin Python runtime for xr_client, runtime debug tools, calibration helpers and capture_client.
    python3 python3-venv python3-yaml python3-numpy python3-opencv

    # XREAL camera/HID/USB access for capture_service_cpp and display/helper tools.
    v4l-utils usbutils udev
    libhidapi-hidraw0 libhidapi-libusb0 libusb-1.0-0 libudev1

    # Graphics/window-system runtime libraries used by OpenVR/Monado/overlays.
    # SteamVR and proprietary GPU drivers/ICDs are installed separately.
    libvulkan1 mesa-vulkan-drivers
    libgl1 libglx-mesa0 libegl1 libegl-mesa0 libgbm1 libdrm2
    libx11-6 libxrandr2 libxext6 libxfixes3 libxxf86vm1 libxinerama1 libxi6 libxcursor1
    libxcb1 libxcb-randr0 libxcb-present0 libxcb-dri3-0 libxcb-xfixes0 libxcb-keysyms1
    libxcb-shm0 libxcb-xinput0 libxcb-xkb1
    libwayland-client0 libwayland-cursor0 libwayland-egl1 libxkbcommon0 libxkbcommon-x11-0
    libsdl2-2.0-0 libpipewire-0.3-0 libpulse0

    # Common native runtime libraries frequently needed by packaged C++ binaries.
    libdbus-1-3 libsystemd0 libstdc++6 zlib1g libgcc-s1 libgomp1
  )
  apt_install_required "${runtime_packages[@]}"
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
  log "install udev rules: $UDEV_RULE_PATH"
  "${SUDO[@]}" tee "$UDEV_RULE_PATH" >/dev/null <<'UDEV'
# XREAL Air 2 Ultra runtime access for camera/HID paths.
SUBSYSTEM=="usb", ATTR{idVendor}=="3318", ATTR{idProduct}=="0426", MODE="0660", GROUP="plugdev", TAG+="uaccess"
SUBSYSTEM=="hidraw", ATTRS{idVendor}=="3318", ATTRS{idProduct}=="0426", MODE="0660", GROUP="plugdev", TAG+="uaccess"
SUBSYSTEM=="video4linux", ATTRS{idVendor}=="3318", ATTRS{idProduct}=="0426", MODE="0660", GROUP="video", TAG+="uaccess"
UDEV
  "${SUDO[@]}" udevadm control --reload-rules || true
  "${SUDO[@]}" udevadm trigger || true
fi

create_or_repair_thin_venv() {
  local package_root="$1"
  local venv_dir="$package_root/bin/python-runtime/venv"
  local recreate="${XR_RECREATE_VENV:-0}"

  if [[ "$PACKAGE_MODE" != "1" ]]; then
    log "skip thin venv setup: package root not detected at $package_root"
    return 0
  fi

  mkdir -p "$package_root/bin/python-runtime"
  if [[ "$recreate" == "1" && -d "$venv_dir" ]]; then
    log "remove existing thin venv: $venv_dir"
    rm -rf "$venv_dir"
  fi

  if [[ ! -x "$venv_dir/bin/python" ]]; then
    log "create thin package-local venv with --system-site-packages: $venv_dir"
    python3 -m venv --system-site-packages "$venv_dir"
  else
    log "thin venv already exists: $venv_dir"
  fi

  "$venv_dir/bin/python" - <<'PY'
import sys
print("thin venv python:", sys.executable)
print("python version:", sys.version.split()[0])
PY
}

if [[ "$INSTALL_THIN_VENV" == "1" ]]; then
  create_or_repair_thin_venv "$XR_PACKAGE_ROOT"
fi

run_runtime_checks() {
  local check_python="python3"
  if [[ -x "$XR_PACKAGE_ROOT/bin/python-runtime/venv/bin/python" ]]; then
    check_python="$XR_PACKAGE_ROOT/bin/python-runtime/venv/bin/python"
  fi

  log "runtime Python check: $check_python"
  "$check_python" - <<'PY'
import importlib

# Required for packaged runtime Python code:
#   yaml      -> runtime_debug_viewer config and kalibr_to_basalt calibration helper
#   numpy/cv2 -> startup gate, camera diagnostics and calibration recorder
# capture_client core itself is dependency-free and is imported from package-local PYTHONPATH.
required = ["yaml", "numpy", "cv2"]
failed = []

for name in required:
    try:
        importlib.import_module(name)
        print(f"OK import {name}")
    except Exception as exc:
        print(f"FAIL import {name}: {exc}")
        failed.append(name)

if failed:
    raise SystemExit(
        "missing required Python runtime modules: " + ", ".join(failed) +
        "\nInstall runtime deps again on Ubuntu 24.04."
    )
PY

  if [[ -d "$XR_PACKAGE_ROOT/bin" ]]; then
    log "scan package ELF dependencies for missing shared libraries"
    local missing_file
    missing_file="$(mktemp)"
    while IFS= read -r -d '' f; do
      if file "$f" | grep -q 'ELF'; then
        if ldd "$f" 2>/dev/null | grep -q 'not found'; then
          echo "MISSING for $f" >>"$missing_file"
          ldd "$f" 2>/dev/null | grep 'not found' >>"$missing_file" || true
          echo >>"$missing_file"
        fi
      fi
    done < <(find "$XR_PACKAGE_ROOT/bin" -type f -print0 2>/dev/null)

    if [[ -s "$missing_file" ]]; then
      cat "$missing_file" >&2
      rm -f "$missing_file"
      fatal "some package binaries/libraries have missing shared libraries; install missing runtime packages"
    fi
    rm -f "$missing_file"
    log "ELF dependency scan: OK"
  fi
}

if [[ "$RUN_CHECKS" == "1" ]]; then
  run_runtime_checks
fi

cat >&2 <<EOF2

[install_runtime_deps_ubuntu24] Done.

Notes:
  - Re-login or reboot may be needed for video/input/plugdev group changes.
  - SteamVR itself is not installed by this script.
  - NVIDIA proprietary driver/ICD is not installed by this script.
  - The package-local venv is intentionally thin and uses system site-packages.
  - The removed legacy Python/GStreamer capture_service is not supported by this
    runtime installer. capture_service_cpp is the runtime capture backend;
    capture_client is the only Python capture-side package kept in the artifact.
EOF2
