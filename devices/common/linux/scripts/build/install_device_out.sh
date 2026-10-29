#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
if [[ -z "${XR_BUILD_SESSION_DIR:-}" || ! -d "$XR_BUILD_SESSION_DIR" ]] || [[ -z "${XR_BUILD_SESSION_OWNER:-}" ]] || ! kill -0 "${XR_BUILD_SESSION_OWNER:-0}" 2>/dev/null; then
  exec bash "$SCRIPT_DIR/with_build_session.sh" bash "$0" "$@"
fi
# shellcheck source=/dev/null
if [[ -n "${XR_DEVICE_OUT_ENV:-}" ]]; then
  # shellcheck source=/dev/null
  source "$XR_DEVICE_OUT_ENV"
else
  # shellcheck source=/dev/null
  source "$SCRIPT_DIR/device_out_env.sh"
fi

log() { echo "[install_device_out:${XR_TARGET_DEVICE}] $*" >&2; }

# shellcheck source=build_steps.sh
source "$SCRIPT_DIR/build_steps.sh"

# System preparation is a separate, explicit operation. Keep every component
# (including vendor hooks and optional downloads) unprivileged during this run.
export INSTALL_APT_DEPS=0 INSTALL_LFS_DEPS=0 XRIZER_INSTALL_SYSTEM_DEPS=0 ADD_INPUT_GROUP=0
bash "$SCRIPT_DIR/install_build_deps_ubuntu.sh" --check

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
run_step() {
  local name="$1"; shift
  if ! should_run_step "$name"; then
    log "skip $name due XR_BUILD_ONLY='$XR_BUILD_ONLY'"
    return 0
  fi
  log "== $name =="
  "$@"
}

export ROOT_PROJECT="$XR_ROOT_PROJECT"
export XR_BIN_ROOT="$XR_OUT_BIN_ROOT"
export XR_TARGET_DEVICE="${XR_TARGET_DEVICE:-generic}"
export XR_DEVICE_TARGET="${XR_DEVICE_TARGET:-$XR_TARGET_DEVICE}"
mkdir -p "$XR_OUT_BIN_ROOT"

python3 "$SCRIPT_DIR/../runtime/cpu_target.py" check-output "$XR_OUT_ROOT" "$XR_CPU_PROFILE_FILE"

# Even jobs without OpenCV consumers must match the SDK builder in native mode.
# Otherwise the merged package could contain incompatible native CPU targets.
if [[ "$XR_BUILD_CPU" == native && -n "${XR_OPENCV_PREFIX:-}" ]]; then
  bash "$SCRIPT_DIR/build_opencv.sh"
fi

if [[ "${XR_USE_BUNDLED_OPENCV:-1}" == "1" ]]; then
  for opencv_step in capture_service_cpp basalt_vio mercury_hand_tracking xr_spatial monado_driver; do
    if should_run_step "$opencv_step"; then
      source "$SCRIPT_DIR/opencv_env.sh"
      mkdir -p "$XR_OUT_BIN_ROOT/opencv/licenses"
      cp -a "$XR_OPENCV_PREFIX/licenses/." "$XR_OUT_BIN_ROOT/opencv/licenses/"
      break
    fi
  done
fi

# This script centralizes output paths for install/build steps. Individual
# install scripts remain usable directly, but this is the recommended device
# package build entrypoint.

run_step capture_service_cpp env \
  XR_CAPTURE_SERVICE_CPP_DEVICE="${XR_CAPTURE_SERVICE_CPP_DEVICE:-$XR_TARGET_DEVICE}" \
  INSTALL_BIN_DIR="$XR_OUT_BIN_ROOT/capture_service_cpp" \
  BUILD_DIR="$XR_BUILD_ROOT/capture_service_cpp" \
  bash "$XR_ROOT_PROJECT/capture_service_cpp/scripts/linux/build_capture_service_cpp.sh"

if [[ "${XR_BUILD_CAPTURE_SERVICE_PYTHON:-0}" == "1" || " ${XR_BUILD_ONLY:-} " == *" capture_service_python "* ]]; then
  log "capture_service_python was removed from the core tree; use capture_service_cpp or restore the legacy component separately"
  exit 2
else
  log "skip capture_service_python: legacy Python/GStreamer capture_service removed; capture_service_cpp is the default"
fi

if [[ -n "${XR_DEVICE_BUILD_HOOK:-}" ]]; then
  if vendor_components_enabled; then
    if [[ ! -x "$XR_DEVICE_BUILD_HOOK" ]]; then
      log "vendor build hook is not executable: $XR_DEVICE_BUILD_HOOK"
      exit 2
    fi
    log "== vendor components: ${XR_VENDOR_COMPONENTS:-$XR_TARGET_DEVICE} =="
    "$XR_DEVICE_BUILD_HOOK"
  else
    log "skip vendor components: XR_BUILD_VENDOR_COMPONENTS=${XR_BUILD_VENDOR_COMPONENTS:-0}"
  fi
fi

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
  ORT_ARCHIVE="$XR_DEPENDENCY_CACHE_ROOT/downloads/onnxruntime-linux-x64-1.18.1.tgz" \
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

run_step openvr_driver bash "$XR_ROOT_PROJECT/drivers/build_profiles.sh" steamvr

if should_build_xrizer; then
  run_step xrizer env \
    INSTALL_BIN_DIR="$XR_OUT_BIN_ROOT/drivers/xrizer" \
    XR_BIN_ROOT="$XR_OUT_BIN_ROOT" \
    XRIZER_INSTALL_RUSTUP="${XRIZER_INSTALL_RUSTUP:-1}" \
    bash "$XR_ROOT_PROJECT/drivers/xrizer/scripts/linux/install_xrizer.sh"
else
  log "skip xrizer: optional GPL component disabled; set XR_BUILD_XRIZER=1 or XR_BUILD_ONLY=xrizer to build it"
fi

run_step monado_driver bash "$XR_ROOT_PROJECT/drivers/build_profiles.sh" monado

run_step steamvr_video_overlay env \
  INSTALL_BIN_DIR="$XR_OUT_BIN_ROOT/apps/steamvr/video_overlay" \
  BUILD_DIR="${STEAMVR_VIDEO_OVERLAY_BUILD_DIR:-$XR_BUILD_ROOT/apps/steamvr/video_overlay}" \
  XR_OPENVR_SDK_ROOT="${XR_OPENVR_SDK_ROOT:-${XR_OPENVR_SDK_DIR:-$XR_ROOT_PROJECT/third_party/openvr}}" \
  bash "$XR_ROOT_PROJECT/apps/steamvr/video_overlay/scripts/linux/install_steamvr_video_overlay.sh"

run_step steamvr_spatial_overlay env \
  INSTALL_BIN_DIR="$XR_OUT_BIN_ROOT/apps/steamvr/spatial_overlay" \
  BUILD_DIR="${STEAMVR_SPATIAL_OVERLAY_BUILD_DIR:-$XR_BUILD_ROOT/apps/steamvr/spatial_overlay}" \
  XR_OPENVR_SDK_DIR="${XR_OPENVR_SDK_DIR:-${XR_OPENVR_SDK_ROOT:-$XR_ROOT_PROJECT/third_party/openvr}}" \
  bash "$XR_ROOT_PROJECT/apps/steamvr/spatial_overlay/scripts/linux/install_xr_steamvr_spatial_overlay.sh"

run_step steamvr_spatial_scene env \
  INSTALL_BIN_DIR="$XR_OUT_BIN_ROOT/apps/steamvr/spatial_scene" \
  BUILD_DIR="${STEAMVR_SPATIAL_SCENE_BUILD_DIR:-$XR_BUILD_ROOT/apps/steamvr/spatial_scene}" \
  XR_OPENVR_SDK_DIR="${XR_OPENVR_SDK_DIR:-${XR_OPENVR_SDK_ROOT:-$XR_ROOT_PROJECT/third_party/openvr}}" \
  bash "$XR_ROOT_PROJECT/apps/steamvr/spatial_scene/scripts/linux/install_xr_steamvr_spatial_scene.sh"

# Preserve binaries that were just built directly under XR_OUT_ROOT/bin.
# package_device_out.sh normally cleans XR_OUT_ROOT when used standalone,
# but that would delete the freshly built package binaries in this build path.
# Partial CI/dev targets such as XR_BUILD_ONLY="drivers" intentionally do not
# rebuild every runtime prerequisite, so package sanity checks must warn instead
# of failing unless the caller asks for strict packaging.
if [[ -n "${XR_BUILD_ONLY:-}" ]]; then
  XR_PACKAGE_ALLOW_PARTIAL="${XR_PACKAGE_ALLOW_PARTIAL:-1}"
else
  XR_PACKAGE_ALLOW_PARTIAL="${XR_PACKAGE_ALLOW_PARTIAL:-0}"
fi
XR_PACKAGE_CLEAN=0 XR_PACKAGE_ALLOW_PARTIAL="$XR_PACKAGE_ALLOW_PARTIAL" XR_PACKAGE_SOURCE_BIN_ROOT="$XR_OUT_BIN_ROOT" \
  "${XR_DEVICE_PACKAGE_SCRIPT:-$SCRIPT_DIR/package_device_out.sh}"
log "done: $XR_OUT_ROOT"
