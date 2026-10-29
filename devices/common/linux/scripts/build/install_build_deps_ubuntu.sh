#!/usr/bin/env bash
# Prepare system dependencies before the unprivileged XR Gate build.
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=build_steps.sh
source "$SCRIPT_DIR/build_steps.sh"

log() { echo "[build-deps] $*" >&2; }
fail() { echo "[build-deps][ERROR] $*" >&2; exit 2; }
usage() {
  cat <<'USAGE'
Install Ubuntu 24.04 / 26.04 build dependencies before compiling XR Gate.

Usage:
  devices/common/linux/scripts/build/install_build_deps_ubuntu.sh [--check|--list]

With no arguments, install packages in one sudo invocation (or directly as root).
  --check  Report missing packages and exit without changing the system.
  --list   Print required package names without installing anything.
  --help   Show this help.

Use the same XR_BUILD_ONLY, XR_BUILD_VENDOR_COMPONENTS, XR_BUILD_XRIZER and
XR_DOWNLOAD_HAND_TRACKING_MODELS settings as the subsequent build. By default,
prepare the complete package, including XREAL support. Optional xrizer and
hand-tracking model dependencies are included only when selected.

Device permissions/udev rules and the input group belong to runtime setup and
are not changed by this script. Rustup/Cargo tools are installed by the optional
xrizer component into the user's home without sudo.
USAGE
}

mode=install
if [[ $# -gt 0 ]]; then
  [[ $# -eq 1 ]] || fail "expected at most one option"
  case "$1" in
    --check) mode=check ;;
    --list) mode=list ;;
    -h|--help) usage; exit 0 ;;
    *) fail "unknown option: $1" ;;
  esac
fi

packages=()
declare -A seen=()
add_packages() {
  local package
  for package in "$@"; do
    if [[ -z "${seen[$package]:-}" ]]; then
      packages+=("$package")
      seen[$package]=1
    fi
  done
}

# Source/download/package helpers used even by partial builds.
add_packages bash coreutils findutils git ca-certificates curl wget tar gzip unzip rsync jq file binutils python3 python3-venv libseccomp2

for step in capture_service_cpp basalt_vio imu_3dof mercury_hand_tracking xr_runtime_adapter \
  override_controller xr_video xr_spatial bridges openvr_driver monado_driver \
  steamvr_video_overlay steamvr_spatial_overlay steamvr_spatial_scene; do
  if should_run_step "$step"; then
    add_packages build-essential cmake ninja-build pkg-config ccache nasm libcli11-dev nlohmann-json3-dev
    break
  fi
done

for step in capture_service_cpp xr_spatial mercury_hand_tracking monado_driver; do
  if should_run_step "$step"; then
    add_packages libopencv-dev libhidapi-dev libeigen3-dev
    break
  fi
done

if should_run_step basalt_vio; then
  add_packages zip python3-tk autoconf automake autoconf-archive libtool bison flex \
    libglu1-mesa-dev mesa-common-dev libgl1-mesa-dev
fi

if should_run_step mercury_hand_tracking; then
  add_packages python3-pip libceres-dev libcjson-dev libjson-c-dev \
    libudev-dev libusb-1.0-0-dev libvulkan-dev glslang-tools \
    libx11-dev libxrandr-dev libxxf86vm-dev libxcb-randr0-dev libx11-xcb-dev \
    libwayland-dev libxinerama-dev libgl1-mesa-dev libegl1-mesa-dev
fi

if should_run_step basalt_vio || should_run_step mercury_hand_tracking || should_run_step monado_driver; then
  add_packages libboost-all-dev libceres-dev libtbb-dev libfmt-dev libspdlog-dev
fi

if should_run_step monado_driver; then
  add_packages libvulkan-dev vulkan-tools glslang-tools \
    libx11-dev libx11-xcb-dev libxrandr-dev libxext-dev libxfixes-dev libxxf86vm-dev \
    libxcb1-dev libxcb-randr0-dev libxcb-present-dev libxcb-dri3-dev libxcb-xfixes0-dev libxcb-keysyms1-dev \
    libudev-dev libwayland-dev wayland-protocols libxkbcommon-dev libssl-dev libsystemd-dev libdbus-1-dev
fi

for step in openvr_driver steamvr_video_overlay steamvr_spatial_overlay steamvr_spatial_scene; do
  if should_run_step "$step"; then
    add_packages mesa-common-dev libopengl-dev libgl-dev libglx-dev libegl-dev libvulkan-dev libsdl2-dev \
      libx11-dev libxrandr-dev libxext-dev libxfixes-dev libxi-dev libxcursor-dev libxinerama-dev libxxf86vm-dev \
      libxcb1-dev libxcb-randr0-dev libxcb-present-dev libxcb-dri3-dev libxcb-xfixes0-dev
    break
  fi
done

if vendor_components_enabled; then
  for vendor in ${XR_VENDOR_COMPONENTS:-xreal_ultra}; do
    if [[ "$vendor" == xreal_ultra ]] && { should_run_step device || should_run_step device_components || should_run_step xreal_display_helper; }; then
      add_packages build-essential cmake pkg-config libhidapi-dev
    fi
  done
fi
if should_build_xrizer && should_run_step xrizer; then
  add_packages build-essential pkg-config clang libclang-dev glslc
fi
if should_download_hand_tracking_models; then
  add_packages git-lfs
fi

if [[ "$mode" == list ]]; then
  printf '%s\n' "${packages[@]}"
  exit 0
fi
command -v dpkg-query >/dev/null 2>&1 || fail "dpkg-query is required; this preparer supports Ubuntu 24.04 / 26.04"

if [[ "$mode" == check ]]; then
  installed="$(dpkg-query -W -f='${Package}\t${db:Status-Status}\n' "${packages[@]}" 2>/dev/null || true)"
  declare -A available=()
  while IFS=$'\t' read -r package status; do
    if [[ -n "$package" && "$status" == installed ]]; then
      available[$package]=1
    fi
  done <<< "$installed"
  missing=()
  for package in "${packages[@]}"; do
    [[ -n "${available[$package]:-}" ]] || missing+=("$package")
  done
  if [[ ${#missing[@]} -gt 0 ]]; then
    log "Missing build dependencies: ${missing[*]}"
    fail "Run ./devices/common/linux/scripts/build/install_build_deps_ubuntu.sh before building (with the same build selection)."
  fi
  log "Build dependencies are installed; compilation will not install system packages."
  exit 0
fi

command -v apt-get >/dev/null 2>&1 || fail "apt-get is required; this preparer supports Ubuntu 24.04 / 26.04"
sudo_cmd=()
if [[ "$(id -u)" -ne 0 ]]; then
  command -v sudo >/dev/null 2>&1 || fail "sudo is required to install build dependencies"
  sudo_cmd=(sudo)
fi
log "Installing ${#packages[@]} build dependencies before compilation."
# One elevated process covers both apt operations, regardless of sudo's timeout.
"${sudo_cmd[@]}" bash -c '
  set -euo pipefail
  apt-get update
  DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends "$@"
' install_build_deps "${packages[@]}"
log "Preparation complete. Run the build as your normal user."
