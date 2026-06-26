#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=/dev/null
source "$SCRIPT_DIR/xreal_ultra_out_env.sh"

log() { echo "[install_xreal_ultra_out] $*" >&2; }

# XR_BUILD_ONLY accepts both concrete step names and a few aggregate targets.
# This keeps CI/resource-constrained builds convenient without changing the
# default full-device build. Examples:
#   XR_BUILD_ONLY="capture_service_cpp"
#   XR_BUILD_ONLY="drivers"              # openvr_driver + monado_driver; xrizer is GPL/optional
#   XR_BUILD_ONLY="xrizer"               # only optional OpenVR->OpenXR compatibility layer
#   XR_BUILD_ONLY="hand_tracking_models" # only download Mercury ONNX hand models
#   XR_BUILD_XRIZER=1 XR_BUILD_ONLY="drivers"  # drivers plus optional xrizer
#   XR_DOWNLOAD_HAND_TRACKING_MODELS=1    # opt-in model download during a normal build
#   XR_BUILD_ONLY="capture_service_cpp drivers"
should_run_step() {
  local name="$1"
  local token

  if [[ -z "${XR_BUILD_ONLY:-}" ]]; then
    return 0
  fi

  for token in ${XR_BUILD_ONLY}; do
    case "$token" in
      all|everything) return 0 ;;
      "$name") return 0 ;;
      capture_service|capture_service_native|capture_cpp)
        [[ "$name" == "capture_service_cpp" ]] && return 0
        ;;
      drivers)
        case "$name" in
          openvr_driver|monado_driver) return 0 ;;
          xrizer) [[ "${XR_BUILD_XRIZER:-0}" == "1" ]] && return 0 ;;
        esac
        ;;
      steamvr|steam_vr)
        [[ "$name" == "openvr_driver" ]] && return 0
        ;;
      openxr)
        [[ "$name" == "monado_driver" ]] && return 0
        ;;
      gpl_tools|optional_gpl|drivers_with_xrizer|drivers_gpl)
        case "$name" in
          xrizer) return 0 ;;
          openvr_driver|monado_driver)
            [[ "$token" == drivers_with_xrizer || "$token" == drivers_gpl ]] && return 0
            ;;
        esac
        ;;
    esac
  done

  return 1
}

run_step() {
  local name="$1"; shift
  if ! should_run_step "$name"; then
    log "skip $name due XR_BUILD_ONLY='$XR_BUILD_ONLY'"
    return 0
  fi
  log "== $name =="
  "$@"
}

should_build_xrizer() {
  local token

  if [[ "${XR_BUILD_XRIZER:-0}" == "1" ]]; then
    return 0
  fi

  for token in ${XR_BUILD_ONLY:-}; do
    case "$token" in
      xrizer|gpl_tools|optional_gpl|drivers_with_xrizer|drivers_gpl)
        return 0
        ;;
    esac
  done

  return 1
}

should_download_hand_tracking_models() {
  local token

  if [[ "${XR_DOWNLOAD_HAND_TRACKING_MODELS:-0}" == "1" || "${XR_DOWNLOAD_MERCURY_MODELS:-0}" == "1" ]]; then
    return 0
  fi

  for token in ${XR_BUILD_ONLY:-}; do
    case "$token" in
      hand_tracking_models|mercury_models|hand_models|models)
        return 0
        ;;
    esac
  done

  return 1
}

export ROOT_PROJECT="$XR_ROOT_PROJECT"
export XR_BIN_ROOT="$XR_OUT_BIN_ROOT"
export XR_TARGET_DEVICE="${XR_TARGET_DEVICE:-xreal_ultra}"
export XR_DEVICE_TARGET="${XR_DEVICE_TARGET:-$XR_TARGET_DEVICE}"
mkdir -p "$XR_OUT_BIN_ROOT"

# This script centralizes output paths for install/build steps. Individual
# install scripts remain usable directly, but this is the recommended device
# package build entrypoint.

run_step capture_service_cpp env \
  XR_CAPTURE_SERVICE_CPP_DEVICE="${XR_CAPTURE_SERVICE_CPP_DEVICE:-$XR_TARGET_DEVICE}" \
  INSTALL_BIN_DIR="$XR_OUT_BIN_ROOT/capture_service_cpp" \
  BUILD_DIR="$XR_ROOT_PROJECT/build/capture_service_cpp" \
  bash "$XR_ROOT_PROJECT/capture_service_cpp/scripts/linux/build_capture_service_cpp.sh"

if [[ "${XR_BUILD_CAPTURE_SERVICE_PYTHON:-0}" == "1" || " ${XR_BUILD_ONLY:-} " == *" capture_service_python "* ]]; then
  log "capture_service_python was removed from the core tree; use capture_service_cpp or restore the legacy component separately"
  exit 2
else
  log "skip capture_service_python: legacy Python/GStreamer capture_service removed; capture_service_cpp is the default"
fi

run_step xreal_display_helper env \
  INSTALL_BIN_DIR="$XR_OUT_BIN_ROOT/xreal_display_helper" \
  bash "$XR_ROOT_PROJECT/tools/xreal_ultra/xreal_display_helper/scripts/linux/install_xreal_helper.sh"

run_step basalt_vio env \
  INSTALL_BIN_DIR="$XR_OUT_BIN_ROOT/backends/basalt_vio" \
  INSTALL_LIB_DIR="$XR_OUT_BIN_ROOT/backends/basalt_vio/lib" \
  bash "$XR_ROOT_PROJECT/backends/basalt_vio/scripts/linux/install_basalt.sh"

run_step imu_3dof env \
  INSTALL_BIN_DIR="$XR_OUT_BIN_ROOT/backends/imu_3dof" \
  bash "$XR_ROOT_PROJECT/backends/imu_3dof/scripts/linux/install_imu_3dof.sh"

run_step mercury_hand_tracking env \
  INSTALL_BIN_DIR="$XR_OUT_BIN_ROOT/backends/mercury_hand_tracking" \
  ORT_ROOT="$XR_OUT_BIN_ROOT/onnxruntime/onnxruntime-linux-x64-1.18.1" \
  ORT_ARCHIVE="$XR_OUT_BIN_ROOT/onnxruntime/onnxruntime-linux-x64-1.18.1.tgz" \
  MERCURY_MODELS="$XR_OUT_BIN_ROOT/hand-tracking-models/mercury" \
  INSTALL_MERCURY_MODELS=0 \
  bash "$XR_ROOT_PROJECT/backends/mercury_hand_tracking/scripts/linux/install_mercury.sh"

if should_download_hand_tracking_models; then
  log "== hand_tracking_models =="
  env \
    ROOT_PROJECT="$XR_ROOT_PROJECT" \
    XR_BIN_ROOT="$XR_OUT_BIN_ROOT" \
    MERCURY_MODELS="$XR_OUT_BIN_ROOT/hand-tracking-models/mercury" \
    bash "$XR_ROOT_PROJECT/backends/mercury_hand_tracking/scripts/linux/download_mercury_models.sh"
else
  log "skip hand_tracking_models: set XR_DOWNLOAD_HAND_TRACKING_MODELS=1 or XR_BUILD_ONLY=hand_tracking_models to download optional ONNX models"
fi

run_step xr_runtime_adapter env \
  INSTALL_BIN_DIR="$XR_OUT_BIN_ROOT/runtime_adapters/xr_runtime_adapter" \
  bash "$XR_ROOT_PROJECT/runtime_adapters/xr_runtime_adapter/scripts/linux/install_xr_runtime_adapter.sh"

run_step override_controller env \
  INSTALL_BIN_DIR="$XR_OUT_BIN_ROOT/override_controller" \
  bash "$XR_ROOT_PROJECT/override_controller/scripts/linux/install_override_controller.sh"

run_step xr_video env \
  INSTALL_BIN_DIR="$XR_OUT_BIN_ROOT/backends/xr_video" \
  bash "$XR_ROOT_PROJECT/backends/xr_video/scripts/linux/install_xr_video.sh"

run_step xr_spatial env \
  INSTALL_BIN_DIR="$XR_OUT_BIN_ROOT/backends/xr_spatial" \
  INSTALL_RUNTIME_BUNDLE=0 \
  INSTALL_LEGACY_SPATIAL_MAPPER_COMPAT=0 \
  bash "$XR_ROOT_PROJECT/backends/xr_spatial/scripts/linux/install_xr_spatial.sh"

run_step bridges env \
  INSTALL_BIN_DIR="$XR_OUT_BIN_ROOT/bridges" \
  bash "$XR_ROOT_PROJECT/bridges/scripts/linux/install_bridges.sh"

run_step openvr_driver bash -c '
  set -euo pipefail

  normalize_display_frequency_hz() {
    local value="${1:-60}"
    python3 - "$value" <<'"'"'PY'"'"'
import math
import sys
text = sys.argv[1].strip()
try:
    value = float(text)
except ValueError:
    print(f"[ERROR] Unsupported display frequency: {text}", file=sys.stderr)
    print("Expected integer Hz in range 60..120.", file=sys.stderr)
    sys.exit(2)
if not math.isfinite(value) or abs(value - round(value)) > 1e-6:
    print(f"[ERROR] Unsupported display frequency: {text}", file=sys.stderr)
    print("Expected integer Hz in range 60..120.", file=sys.stderr)
    sys.exit(2)
hz = int(round(value))
if hz < 60 or hz > 120:
    print(f"[ERROR] Unsupported display frequency: {text}", file=sys.stderr)
    print("Expected integer Hz in range 60..120.", file=sys.stderr)
    sys.exit(2)
print(hz)
PY
  }

  normalize_display_mode() {
    local value="${1:-direct}"
    value="${value,,}"
    value="${value//-/_}"
    case "$value" in
      direct|direct_mode|drm|drm_lease) printf "direct\n" ;;
      extended|extended_sbs|sbs|windowed|desktop) printf "extended_sbs\n" ;;
      *)
        echo "[ERROR] Unsupported OpenVR display mode: $1" >&2
        echo "Expected direct or extended_sbs." >&2
        exit 2
        ;;
    esac
  }

  openvr_driver_dir_name() {
    local hz="$1"
    local mode="$2"
    if [[ "$mode" == "direct" ]]; then
      printf "openvr_driver_%sHZ\n" "$hz"
    else
      printf "openvr_driver_%sHZ_%s\n" "$hz" "$mode"
    fi
  }

  XR_OPENVR_DEVICE="${XR_OPENVR_DEVICE:-${XR_DEVICE_TARGET:-xreal_ultra}}"

  readarray -t freqs < <(for raw in ${XR_OPENVR_BUILD_FREQUENCIES:-60 75 90}; do normalize_display_frequency_hz "$raw"; done | awk "!seen[\$0]++")
  readarray -t modes < <(for raw in ${XR_OPENVR_BUILD_MODES:-direct}; do normalize_display_mode "$raw"; done | awk "!seen[\$0]++")

  for mode in "${modes[@]}"; do
    for freq in "${freqs[@]}"; do
      driver_dir="$(openvr_driver_dir_name "$freq" "$mode")"
      echo "[install_xreal_ultra_out] build OpenVR driver ${freq}Hz mode=${mode} -> ${XR_OUT_BIN_ROOT}/drivers/${driver_dir}" >&2
      XR_OPENVR_DEVICE="$XR_OPENVR_DEVICE" \
      XR_OPENVR_DISPLAY_FREQUENCY_HZ="$freq" \
      XR_OPENVR_DISPLAY_MODE="$mode" \
      XR_OPENVR_DRIVER_DIR_NAME="$driver_dir" \
      XR_OPENVR_SINGLE_VARIANT_BUILD=1 \
      INSTALL_DRIVERS_ROOT="$XR_OUT_BIN_ROOT/drivers" \
      INSTALL_DRIVER_ROOT="$XR_OUT_BIN_ROOT/drivers/$driver_dir" \
      INSTALL_DRIVER_PACKAGE="$XR_OUT_BIN_ROOT/drivers/$driver_dir/xr_tracking" \
      BUILD_DIR="${OPENVR_BUILD_ROOT:-$XR_ROOT_PROJECT/build/drivers}/$driver_dir" \
      CLONE_OPENVR="${CLONE_OPENVR:-1}" \
        "$XR_ROOT_PROJECT/drivers/openvr_driver/scripts/build_driver.sh"
    done
  done
'

if should_build_xrizer; then
  run_step xrizer env \
    INSTALL_BIN_DIR="$XR_OUT_BIN_ROOT/drivers/xrizer" \
    XR_BIN_ROOT="$XR_OUT_BIN_ROOT" \
    XRIZER_INSTALL_RUSTUP="${XRIZER_INSTALL_RUSTUP:-1}" \
    bash "$XR_ROOT_PROJECT/drivers/xrizer/scripts/linux/install_xrizer.sh"
else
  log "skip xrizer: optional GPL component disabled; set XR_BUILD_XRIZER=1 or XR_BUILD_ONLY=xrizer to build it"
fi

run_step monado_driver env \
  XR_MONADO_DEVICE="${XR_MONADO_DEVICE:-$XR_TARGET_DEVICE}" \
  BIN_DIR="$XR_OUT_BIN_ROOT/drivers/monado_driver" \
  XR_BIN_ROOT="$XR_OUT_BIN_ROOT" \
  BUILD_DIR="${MONADO_BUILD_DIR:-$XR_ROOT_PROJECT/third_party/monado_driver/build/xr_tracking_relwithdebinfo}" \
  CLONE_MONADO="${CLONE_MONADO:-1}" \
  bash "$XR_ROOT_PROJECT/drivers/monado_driver/scripts/linux/install.sh"

run_step steamvr_video_overlay env \
  INSTALL_BIN_DIR="$XR_OUT_BIN_ROOT/apps/steamvr/video_overlay" \
  BUILD_DIR="${STEAMVR_VIDEO_OVERLAY_BUILD_DIR:-$XR_ROOT_PROJECT/build/apps/steamvr/video_overlay/relwithdebinfo}" \
  XR_OPENVR_SDK_ROOT="${XR_OPENVR_SDK_ROOT:-${XR_OPENVR_SDK_DIR:-$XR_ROOT_PROJECT/third_party/openvr}}" \
  bash "$XR_ROOT_PROJECT/apps/steamvr/video_overlay/scripts/linux/install_steamvr_video_overlay.sh"

run_step steamvr_spatial_overlay env \
  INSTALL_BIN_DIR="$XR_OUT_BIN_ROOT/apps/steamvr/spatial_overlay" \
  BUILD_DIR="${STEAMVR_SPATIAL_OVERLAY_BUILD_DIR:-$XR_ROOT_PROJECT/build/apps/steamvr/spatial_overlay/relwithdebinfo}" \
  XR_OPENVR_SDK_DIR="${XR_OPENVR_SDK_DIR:-${XR_OPENVR_SDK_ROOT:-$XR_ROOT_PROJECT/third_party/openvr}}" \
  bash "$XR_ROOT_PROJECT/apps/steamvr/spatial_overlay/scripts/linux/install_xr_steamvr_spatial_overlay.sh"

run_step steamvr_spatial_scene env \
  INSTALL_BIN_DIR="$XR_OUT_BIN_ROOT/apps/steamvr/spatial_scene" \
  BUILD_DIR="${STEAMVR_SPATIAL_SCENE_BUILD_DIR:-$XR_ROOT_PROJECT/build/apps/steamvr/spatial_scene/relwithdebinfo}" \
  XR_OPENVR_SDK_DIR="${XR_OPENVR_SDK_DIR:-${XR_OPENVR_SDK_ROOT:-$XR_ROOT_PROJECT/third_party/openvr}}" \
  bash "$XR_ROOT_PROJECT/apps/steamvr/spatial_scene/scripts/linux/install_xr_steamvr_spatial_scene.sh"

# Preserve binaries that were just built directly under XR_OUT_ROOT/bin.
# package_xreal_ultra_out.sh normally cleans XR_OUT_ROOT when used standalone,
# but that would delete the freshly built package binaries in this build path.
# Partial CI/dev targets such as XR_BUILD_ONLY="drivers" intentionally do not
# rebuild every runtime prerequisite, so package sanity checks must warn instead
# of failing unless the caller asks for strict packaging.
if [[ -n "${XR_BUILD_ONLY:-}" ]]; then
  XR_PACKAGE_ALLOW_PARTIAL="${XR_PACKAGE_ALLOW_PARTIAL:-1}"
else
  XR_PACKAGE_ALLOW_PARTIAL="${XR_PACKAGE_ALLOW_PARTIAL:-0}"
fi
XR_PACKAGE_CLEAN=0 XR_PACKAGE_ALLOW_PARTIAL="$XR_PACKAGE_ALLOW_PARTIAL" XR_PACKAGE_SOURCE_BIN_ROOT="$XR_OUT_BIN_ROOT"   "$SCRIPT_DIR/package_xreal_ultra_out.sh"
log "done: $XR_OUT_ROOT"
