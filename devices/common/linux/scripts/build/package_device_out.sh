#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=/dev/null
if [[ -n "${XR_DEVICE_OUT_ENV:-}" ]]; then
  # shellcheck source=/dev/null
  source "$XR_DEVICE_OUT_ENV"
else
  # shellcheck source=/dev/null
  source "$SCRIPT_DIR/device_out_env.sh"
fi

log() { echo "[package_device_out:${XR_TARGET_DEVICE}] $*" >&2; }
fatal() { echo "[package_device_out:${XR_TARGET_DEVICE}][ERROR] $*" >&2; exit 1; }

copy_dir() {
  local src="$1"
  local dst="$2"
  if [[ ! -e "$src" ]]; then
    log "skip missing: $src"
    return 0
  fi
  mkdir -p "$(dirname "$dst")"
  if [[ -d "$dst" ]]; then
    local src_real dst_real
    src_real="$(readlink -f "$src")"
    dst_real="$(readlink -f "$dst")"
    if [[ "$src_real" == "$dst_real" ]]; then
      log "already in place: $dst"
      return 0
    fi
  fi
  rm -rf "$dst"
  cp -a "$src" "$dst"
}

copy_file() {
  local src="$1"
  local dst="$2"
  if [[ ! -f "$src" ]]; then
    log "skip missing file: $src"
    return 0
  fi
  mkdir -p "$(dirname "$dst")"
  cp -a "$src" "$dst"
}

copy_if_exists() {
  local src="$1"
  local dst="$2"
  if [[ -d "$src" ]]; then
    copy_dir "$src" "$dst"
  elif [[ -f "$src" ]]; then
    copy_file "$src" "$dst"
  else
    log "skip missing: $src"
  fi
}

copy_device_bundle_for() {
  local target="$1"
  local src="$XR_ROOT_PROJECT/devices/$target"
  local dst="$XR_OUT_ROOT/devices/$target"
  if [[ ! -d "$src" ]]; then
    fatal "configured runtime profile bundle not found: $src"
  fi
  mkdir -p "$dst"
  rsync -a --delete \
    --exclude='/linux/scripts/install_*_out.sh' \
    --exclude='/linux/scripts/package_*_out.sh' \
    --exclude='/linux/scripts/*_out_env.sh' \
    --exclude='/linux/scripts/run_*_act_build.sh' \
    --exclude='/linux/scripts/unpack_*.sh' \
    --exclude='/linux/scripts/build_*_components.sh' \
    --exclude='/linux/scripts/package_*_components.sh' \
    --exclude='__pycache__/' \
    --exclude='*.pyc' \
    --exclude='*.pyo' \
    "$src/" "$dst/"
}

copy_device_bundles() {
  local target
  local targets="${XR_PACKAGE_DEVICE_TARGETS:-$XR_TARGET_DEVICE}"
  [[ -n "$targets" ]] || fatal "XR_PACKAGE_DEVICE_TARGETS is empty"
  for target in $targets; do
    copy_device_bundle_for "$target"
  done
}

copy_common_device_bundle() {
  local src="$XR_ROOT_PROJECT/devices/common"
  local dst="$XR_OUT_ROOT/devices/common"
  if [[ ! -d "$src" ]]; then
    fatal "common device runtime not found: $src"
  fi
  mkdir -p "$dst"
  rsync -a --delete \
    --exclude='/linux/scripts/build/' \
    --exclude='/linux/scripts/ci/' \
    --exclude='/linux/scripts/release/' \
    --exclude='__pycache__/' \
    --exclude='*.pyc' \
    --exclude='*.pyo' \
    "$src/" "$dst/"
}

write_monado_openxr_runtime_manifest() {
  local monado_bin_dir="$XR_OUT_BIN_ROOT/drivers/monado_driver"
  local openxr_lib="$monado_bin_dir/libopenxr_monado.so"
  local manifest="$monado_bin_dir/openxr_monado_xrgate.json"
  local env_script="$monado_bin_dir/openxr_runtime_env.sh"

  if [[ ! -e "$openxr_lib" ]]; then
    log "skip Monado OpenXR runtime manifest: lib not present: $openxr_lib"
    return 0
  fi

  mkdir -p "$monado_bin_dir"
  cat > "$manifest" <<'EOF'
{
  "file_format_version": "1.0.0",
  "runtime": {
    "name": "XR Gate Monado",
    "library_path": "./libopenxr_monado.so"
  }
}
EOF

  cat > "$env_script" <<'EOF'
#!/usr/bin/env bash
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export XR_RUNTIME_JSON="$SCRIPT_DIR/openxr_monado_xrgate.json"
echo "XR_RUNTIME_JSON=$XR_RUNTIME_JSON"
EOF
  chmod 0755 "$env_script"
  log "wrote Monado OpenXR runtime manifest: $manifest"
}

copy_runtime_py_dir() {
  local src="$1"
  local dst="$2"
  if [[ ! -d "$src" ]]; then
    log "skip missing python dir: $src"
    return 0
  fi
  mkdir -p "$dst"
  rsync -a --delete \
    --exclude='__pycache__/' \
    --exclude='*.pyc' \
    --exclude='*.pyo' \
    --exclude='.pytest_cache/' \
    --exclude='.mypy_cache/' \
    --exclude='.ruff_cache/' \
    --exclude='.git/' \
    "$src/" "$dst/"
}

copy_runtime_file() {
  copy_file "$1" "$2"
  chmod +x "$2" 2>/dev/null || true
}


write_python_runtime_env() {
  mkdir -p "$XR_OUT_BIN_ROOT/python-runtime"
  cat > "$XR_OUT_BIN_ROOT/python-runtime/env.sh" <<'PYENV'
#!/usr/bin/env bash
# Source this file to use the package-local thin Python runtime.
# The venv is intentionally created with --system-site-packages and should not
# contain heavy native packages such as OpenCV, PyGObject, GStreamer bindings or
# NumPy. Those are provided by the target system through apt.
set -euo pipefail
XR_PY_ENV_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
XR_PACKAGE_ROOT="$(cd "$XR_PY_ENV_DIR/../.." && pwd)"
export XR_ROOT_PROJECT="${XR_ROOT_PROJECT:-$XR_PACKAGE_ROOT}"
export ROOT_PROJECT="${ROOT_PROJECT:-$XR_ROOT_PROJECT}"
export XR_BIN_ROOT="${XR_BIN_ROOT:-$XR_PACKAGE_ROOT/bin}"
export XR_PYTHON_ROOT="${XR_PYTHON_ROOT:-$XR_BIN_ROOT/python}"
export XR_PYTHON_RUNTIME_ROOT="${XR_PYTHON_RUNTIME_ROOT:-$XR_BIN_ROOT/python-runtime}"
export XR_PYTHON_VENV="${XR_PYTHON_VENV:-$XR_PYTHON_RUNTIME_ROOT/venv}"
if [[ "${XR_PACKAGE_ALLOW_PYTHON_OVERRIDE:-0}" == "1" && -n "${PYTHON:-}" ]]; then
  export PYTHON
else
  export PYTHON="$XR_PYTHON_VENV/bin/python"
fi
export PYTHONPATH="$XR_PYTHON_ROOT:$XR_PYTHON_ROOT/xr_client:$XR_PYTHON_ROOT/tools:${PYTHONPATH:-}"
PYENV
  chmod +x "$XR_OUT_BIN_ROOT/python-runtime/env.sh"
}

prepare_python_runtime() {
  local enabled="${XR_PACKAGE_PYTHON_RUNTIME:-1}"
  if [[ "$enabled" != "1" ]]; then
    log "skip package-local Python runtime: XR_PACKAGE_PYTHON_RUNTIME=$enabled"
    return 0
  fi

  local python_bin="${XR_PACKAGE_PYTHON_BIN:-python3}"
  local venv_dir="${XR_OUT_PYTHON_VENV:-$XR_OUT_BIN_ROOT/python-runtime/venv}"
  mkdir -p "$(dirname "$venv_dir")"

  if [[ ! -x "$venv_dir/bin/python" ]]; then
    log "creating thin package-local Python venv: $venv_dir"
    "$python_bin" -m venv --system-site-packages "$venv_dir"
  else
    log "using existing package-local Python venv: $venv_dir"
  fi

  write_python_runtime_env

  cat > "$XR_OUT_BIN_ROOT/python-runtime/check_python_runtime.sh" <<'PYCHECK'
#!/usr/bin/env bash
set -euo pipefail
RUNTIME_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=/dev/null
source "$RUNTIME_DIR/env.sh"

if [[ ! -x "$PYTHON" ]]; then
  echo "[check_python_runtime][ERROR] package Python not found: $PYTHON" >&2
  exit 1
fi

"$PYTHON" - <<'PY'
import importlib
import sys

print("python:", sys.executable)
print("version:", sys.version.split()[0])

# Required for packaged runtime Python tools after legacy Python capture_service removal.
# Native capture_service_cpp/xr_video do not require Python HID, PyGObject or GStreamer.
# yaml is kept for runtime_debug_viewer configs and kalibr_to_basalt calibration helper.
required = ("yaml", "numpy", "cv2")
failed = []
for name in required:
    try:
        importlib.import_module(name)
        print(f"import {name}: OK")
    except Exception as exc:
        print(f"import {name}: FAIL: {exc}")
        failed.append(name)

if failed:
    raise SystemExit("missing required Python runtime modules: " + ", ".join(failed))
PY
PYCHECK
  chmod +x "$XR_OUT_BIN_ROOT/python-runtime/check_python_runtime.sh"
}


package_include_xrizer_helpers() {
  local mode="${XR_PACKAGE_INCLUDE_XRIZER_HELPERS:-auto}"
  case "$mode" in
    1|true|yes|on) return 0 ;;
    0|false|no|off) return 1 ;;
    auto)
      [[ -d "$XR_OUT_BIN_ROOT/drivers/xrizer" ]]
      return $?
      ;;
    *)
      fatal "unsupported XR_PACKAGE_INCLUDE_XRIZER_HELPERS=$mode; expected auto, 0 or 1"
      ;;
  esac
}

package_project_legal_files() {
  [[ -f "$XR_ROOT_PROJECT/LICENSE" ]] || fatal "project LICENSE not found: $XR_ROOT_PROJECT/LICENSE"
  copy_file "$XR_ROOT_PROJECT/LICENSE" "$XR_OUT_ROOT/LICENSE"
  copy_file "$XR_ROOT_PROJECT/LICENSE" "$XR_OUT_ROOT/LICENSES/MIT.txt"
  if [[ -f "$XR_ROOT_PROJECT/THIRD_PARTY_NOTICES.md" ]]; then
    copy_file "$XR_ROOT_PROJECT/THIRD_PARTY_NOTICES.md" "$XR_OUT_ROOT/THIRD_PARTY_NOTICES.md"
  fi
}

package_xrizer_compliance_files() {
  local xrizer_dir="$XR_OUT_BIN_ROOT/drivers/xrizer"
  local source_archive
  local source_archive_name

  if [[ ! -d "$xrizer_dir" ]]; then
    rm -f "$XR_OUT_ROOT/LICENSES/GPL-3.0-or-later.txt"
    rm -rf "$XR_OUT_ROOT/SOURCES/xrizer"
    return 0
  fi

  [[ -f "$xrizer_dir/LICENSE.GPL-3.0-or-later" ]] || \
    fatal "xrizer binary is present without GPL license: $xrizer_dir/LICENSE.GPL-3.0-or-later"
  [[ -f "$xrizer_dir/SOURCE.txt" ]] || \
    fatal "xrizer binary is present without source metadata: $xrizer_dir/SOURCE.txt"
  [[ -f "$xrizer_dir/SHA256SUMS.txt" ]] || \
    fatal "xrizer binary is present without compliance checksums: $xrizer_dir/SHA256SUMS.txt"
  [[ -d "$xrizer_dir/source" ]] || \
    fatal "xrizer binary is present without Corresponding Source directory: $xrizer_dir/source"

  source_archive="$(find "$xrizer_dir/source" -maxdepth 1 -type f \
    -name 'xrizer-corresponding-source-*.tar.gz' -print -quit)"
  [[ -n "$source_archive" && -f "$source_archive" ]] || \
    fatal "xrizer binary is present without Corresponding Source archive under $xrizer_dir/source"
  source_archive_name="$(basename "$source_archive")"

  copy_file \
    "$xrizer_dir/LICENSE.GPL-3.0-or-later" \
    "$XR_OUT_ROOT/LICENSES/GPL-3.0-or-later.txt"
  copy_dir "$xrizer_dir/source" "$XR_OUT_ROOT/SOURCES/xrizer"

  # Keep one canonical source archive in the package. The component-local path
  # remains valid through a relative symlink, so SOURCE.txt and SHA256SUMS.txt
  # produced by install_xrizer.sh continue to work without doubling archive size.
  rm -rf "$xrizer_dir/source"
  ln -s ../../../SOURCES/xrizer "$xrizer_dir/source"

  sed \
    "s#Corresponding Source archive: source/#Corresponding Source archive: #" \
    "$xrizer_dir/SOURCE.txt" > "$XR_OUT_ROOT/SOURCES/xrizer/SOURCE.txt"
  cat >> "$XR_OUT_ROOT/SOURCES/xrizer/SOURCE.txt" <<EOF_SOURCE_LOCATIONS
Packaged GPL license: ../../LICENSES/GPL-3.0-or-later.txt
Packaged Corresponding Source: $source_archive_name
Component-local source path: ../../bin/drivers/xrizer/source/$source_archive_name
EOF_SOURCE_LOCATIONS

  (
    cd "$XR_OUT_ROOT"
    sha256sum \
      "LICENSES/GPL-3.0-or-later.txt" \
      "SOURCES/xrizer/$source_archive_name" \
      > "SOURCES/xrizer/SHA256SUMS.txt"
  )

  log "packaged xrizer GPL license and Corresponding Source"
}

copy_runtime_scripts() {
  # Only underlying component launchers called by devices/common wrappers
  # are copied here. Build/install scripts, CMake files and source trees stay out
  # of the deploy package.
  copy_runtime_file \
    "$XR_ROOT_PROJECT/capture_service_cpp/scripts/linux/start_capture_service_cpp.sh" \
    "$XR_OUT_BIN_ROOT/scripts/capture_service_cpp/start_capture_service_cpp.sh"

  copy_runtime_file \
    "$XR_ROOT_PROJECT/backends/basalt_vio/scripts/linux/start_basalt.sh" \
    "$XR_OUT_BIN_ROOT/scripts/backends/basalt_vio/start_basalt.sh"
  copy_runtime_file \
    "$XR_ROOT_PROJECT/backends/imu_3dof/scripts/linux/start_imu_3dof_backend.sh" \
    "$XR_OUT_BIN_ROOT/scripts/backends/imu_3dof/start_imu_3dof_backend.sh"
  copy_runtime_file \
    "$XR_ROOT_PROJECT/backends/mercury_hand_tracking/scripts/linux/start_hand_tracking.sh" \
    "$XR_OUT_BIN_ROOT/scripts/backends/mercury_hand_tracking/start_hand_tracking.sh"
  copy_runtime_file \
    "$XR_ROOT_PROJECT/backends/mercury_hand_tracking/scripts/linux/download_mercury_models.sh" \
    "$XR_OUT_BIN_ROOT/scripts/backends/mercury_hand_tracking/download_mercury_models.sh"
  copy_runtime_file \
    "$XR_ROOT_PROJECT/backends/xr_video/scripts/linux/start_xr_video_backend.sh" \
    "$XR_OUT_BIN_ROOT/scripts/backends/xr_video/start_xr_video_backend.sh"
  copy_runtime_file \
    "$XR_ROOT_PROJECT/backends/xr_video/scripts/linux/start_xr_video_backend_tcp.sh" \
    "$XR_OUT_BIN_ROOT/scripts/backends/xr_video/start_xr_video_backend_tcp.sh"

  copy_runtime_file \
    "$XR_ROOT_PROJECT/backends/xr_spatial/scripts/linux/start_xr_spatial_shm.sh" \
    "$XR_OUT_BIN_ROOT/scripts/backends/xr_spatial/start_xr_spatial_shm.sh"
  copy_runtime_file \
    "$XR_ROOT_PROJECT/backends/xr_spatial/scripts/linux/start_xr_spatial_tcp.sh" \
    "$XR_OUT_BIN_ROOT/scripts/backends/xr_spatial/start_xr_spatial_tcp.sh"
  copy_runtime_file \
    "$XR_ROOT_PROJECT/backends/xr_spatial/scripts/linux/start_xr_spatial_scan.sh" \
    "$XR_OUT_BIN_ROOT/scripts/backends/xr_spatial/start_xr_spatial_scan.sh"
  copy_runtime_file \
    "$XR_ROOT_PROJECT/backends/xr_spatial/scripts/linux/xr_spatial_profile.sh" \
    "$XR_OUT_BIN_ROOT/scripts/backends/xr_spatial/xr_spatial_profile.sh"

  # Runtime launchers live under bin/scripts/backends/<backend>, outside the
  # source tree and outside each backend's portable install bundle. Keep the
  # shared profile resolver next to every launcher that sources it so packaged
  # runs do not depend on backends/common being present as source code.
  local capture_profile_helper="$XR_ROOT_PROJECT/backends/common/scripts/linux/capture_profile.sh"
  local profile_backend
  for profile_backend in \
    basalt_vio \
    imu_3dof \
    mercury_hand_tracking \
    xr_video \
    xr_spatial
  do
    copy_runtime_file \
      "$capture_profile_helper" \
      "$XR_OUT_BIN_ROOT/scripts/backends/$profile_backend/capture_profile.sh"
  done

  copy_runtime_file \
    "$XR_ROOT_PROJECT/runtime_adapters/xr_runtime_adapter/scripts/linux/start_xr_runtime_adapter_shm.sh" \
    "$XR_OUT_BIN_ROOT/scripts/runtime_adapters/xr_runtime_adapter/start_xr_runtime_adapter_shm.sh"
  copy_runtime_file \
    "$XR_ROOT_PROJECT/override_controller/scripts/linux/start_override_controller.sh" \
    "$XR_OUT_BIN_ROOT/scripts/override_controller/start_override_controller.sh"

  copy_runtime_file \
    "$XR_ROOT_PROJECT/bridges/scripts/linux/start_capture_net_bridge.sh" \
    "$XR_OUT_BIN_ROOT/scripts/bridges/start_capture_net_bridge.sh"
  copy_runtime_file \
    "$XR_ROOT_PROJECT/bridges/scripts/linux/start_tracking_udp_bridge.sh" \
    "$XR_OUT_BIN_ROOT/scripts/bridges/start_tracking_udp_bridge.sh"
  copy_runtime_file \
    "$XR_ROOT_PROJECT/bridges/scripts/linux/start_tracking_udp_debug_receiver.sh" \
    "$XR_OUT_BIN_ROOT/scripts/bridges/start_tracking_udp_debug_receiver.sh"

  copy_runtime_file \
    "$XR_ROOT_PROJECT/drivers/openvr_driver/scripts/register_driver.sh" \
    "$XR_OUT_BIN_ROOT/scripts/drivers/openvr_driver/register_driver.sh"
  copy_runtime_file \
    "$XR_ROOT_PROJECT/drivers/steam_vr/scripts/linux/start_openvr_dgpu_direct.sh" \
    "$XR_OUT_BIN_ROOT/scripts/drivers/steam_vr/start_openvr_dgpu_direct.sh"
  copy_runtime_file \
    "$XR_ROOT_PROJECT/drivers/steam_vr/scripts/linux/start_openvr_dgpu_direct_60.sh" \
    "$XR_OUT_BIN_ROOT/scripts/drivers/steam_vr/start_openvr_dgpu_direct_60.sh"
  copy_runtime_file \
    "$XR_ROOT_PROJECT/drivers/monado_driver/scripts/linux/start.sh" \
    "$XR_OUT_BIN_ROOT/scripts/drivers/monado_driver/start.sh"
  if package_include_xrizer_helpers; then
    copy_runtime_file \
      "$XR_ROOT_PROJECT/drivers/xrizer/scripts/linux/register_xrizer_openvrpaths.sh" \
      "$XR_OUT_BIN_ROOT/scripts/drivers/xrizer/register_xrizer_openvrpaths.sh"
    copy_runtime_file \
      "$XR_ROOT_PROJECT/drivers/xrizer/scripts/linux/start_openvr_app_via_monado.sh" \
      "$XR_OUT_BIN_ROOT/scripts/drivers/xrizer/start_openvr_app_via_monado.sh"
    copy_runtime_file \
      "$XR_ROOT_PROJECT/drivers/xrizer/scripts/linux/collect_xrizer_logs.sh" \
      "$XR_OUT_BIN_ROOT/scripts/drivers/xrizer/collect_xrizer_logs.sh"
  else
    log "skip xrizer helper scripts: xrizer binary package not present"
  fi

  copy_runtime_file \
    "$XR_ROOT_PROJECT/apps/steamvr/video_overlay/scripts/linux/start_steamvr_video_overlay.sh" \
    "$XR_OUT_BIN_ROOT/scripts/apps/steamvr/video_overlay/start_steamvr_video_overlay.sh"
  copy_runtime_file \
    "$XR_ROOT_PROJECT/apps/steamvr/spatial_overlay/scripts/linux/start_xr_steamvr_spatial_overlay.sh" \
    "$XR_OUT_BIN_ROOT/scripts/apps/steamvr/spatial_overlay/start_xr_steamvr_spatial_overlay.sh"
  copy_runtime_file \
    "$XR_ROOT_PROJECT/apps/steamvr/spatial_scene/scripts/linux/start_xr_steamvr_spatial_scene.sh" \
    "$XR_OUT_BIN_ROOT/scripts/apps/steamvr/spatial_scene/start_xr_steamvr_spatial_scene.sh"
}

copy_capture_client_runtime() {
  local src="$XR_ROOT_PROJECT/capture_client"
  if [[ ! -d "$src" ]]; then
    fatal "root capture_client package not found: $src"
  fi
  copy_runtime_py_dir "$src" "$XR_OUT_BIN_ROOT/python/capture_client"
}

copy_tools_runtime() {
  copy_file "$XR_ROOT_PROJECT/tools/xr_startup_gate.py" "$XR_OUT_BIN_ROOT/python/tools/xr_startup_gate.py"
  copy_file \
    "$XR_ROOT_PROJECT/runtime_adapters/xr_runtime_adapter/tools/calibrate_hmd_orientation_offset.py" \
    "$XR_OUT_BIN_ROOT/python/tools/calibrate_hmd_orientation_offset.py"
  copy_file "$XR_ROOT_PROJECT/tools/xr_runtime_gesture_watch_debug.py" "$XR_OUT_BIN_ROOT/python/tools/xr_runtime_gesture_watch_debug.py"
  copy_file "$XR_ROOT_PROJECT/tools/debug/view_capture_service_shm.py" "$XR_OUT_BIN_ROOT/python/tools/debug/view_capture_service_shm.py"
  copy_runtime_py_dir "$XR_ROOT_PROJECT/tools/runtime_debug_viewer" "$XR_OUT_BIN_ROOT/python/tools/runtime_debug_viewer"
}


write_root_launcher() {
  local name="$1"
  local target="$2"
  cat > "$XR_OUT_ROOT/$name" <<EOF_LAUNCHER
#!/usr/bin/env bash
set -euo pipefail
PACKAGE_ROOT="\$(cd "\$(dirname "\${BASH_SOURCE[0]}")" && pwd)"
exec "\$PACKAGE_ROOT/$target" "\$@"
EOF_LAUNCHER
  chmod +x "$XR_OUT_ROOT/$name"
}

write_app_launchers() {
  write_root_launcher "calibrate_hmd_orientation_offset.sh" "bin/python/tools/calibrate_hmd_orientation_offset.py"
  write_root_launcher "run_openvr_dgpu_direct.sh" "bin/scripts/drivers/steam_vr/start_openvr_dgpu_direct.sh"
  write_root_launcher "run_openvr_dgpu_direct_60.sh" "bin/scripts/drivers/steam_vr/start_openvr_dgpu_direct_60.sh"
  write_root_launcher "download_mercury_models.sh" "devices/common/linux/scripts/mercury_hand_tracking/download_mercury_models.sh"
  write_root_launcher "run_steamvr_video_overlay.sh" "devices/common/linux/scripts/steamvr_video_overlay/start_steamvr_video_overlay.sh"
  write_root_launcher "run_steamvr_spatial_overlay.sh" "devices/common/linux/scripts/steamvr_spatial_overlay/start_xr_steamvr_spatial_overlay.sh"
  write_root_launcher "run_steamvr_spatial_scene.sh" "devices/common/linux/scripts/steamvr_spatial_scene/start_xr_steamvr_spatial_scene.sh"
  if package_include_xrizer_helpers; then
    write_root_launcher "run_xrizer_register.sh" "bin/scripts/drivers/xrizer/register_xrizer_openvrpaths.sh"
    write_root_launcher "run_xrizer_openvr_app_via_monado.sh" "bin/scripts/drivers/xrizer/start_openvr_app_via_monado.sh"
    write_root_launcher "run_xrizer_collect_logs.sh" "bin/scripts/drivers/xrizer/collect_xrizer_logs.sh"
  fi
}


XR_PACKAGE_CLEAN="${XR_PACKAGE_CLEAN:-1}"
XR_PACKAGE_ALLOW_PARTIAL="${XR_PACKAGE_ALLOW_PARTIAL:-0}"
XR_PACKAGE_COPY_CALIBRATION_DATASET="${XR_PACKAGE_COPY_CALIBRATION_DATASET:-0}"
XR_PACKAGE_COPY_DRIVER_RUNTIME="${XR_PACKAGE_COPY_DRIVER_RUNTIME:-1}"

[[ -d "$XR_ROOT_PROJECT" ]] || fatal "XR_ROOT_PROJECT not found: $XR_ROOT_PROJECT"

log "XR_ROOT_PROJECT=$XR_ROOT_PROJECT"
log "XR_OUT_ROOT=$XR_OUT_ROOT"
log "XR_PACKAGE_SOURCE_BIN_ROOT=$XR_PACKAGE_SOURCE_BIN_ROOT"

if [[ "$XR_PACKAGE_CLEAN" == "1" ]]; then
  rm -rf "$XR_OUT_ROOT"
fi
mkdir -p "$XR_OUT_ROOT"

# Runtime binaries/libraries/assets built by install scripts.
if [[ -d "$XR_PACKAGE_SOURCE_BIN_ROOT" ]]; then
  copy_dir "$XR_PACKAGE_SOURCE_BIN_ROOT" "$XR_OUT_BIN_ROOT"
else
  log "source bin root not found yet, creating empty package bin root: $XR_PACKAGE_SOURCE_BIN_ROOT"
  mkdir -p "$XR_OUT_BIN_ROOT"
fi

package_project_legal_files
package_xrizer_compliance_files

# Hardware-neutral launchers plus all configured runtime profile bundles.
copy_common_device_bundle
copy_device_bundles
write_monado_openxr_runtime_manifest

# Runtime Python entrypoints. They are required at runtime, so keep them under
# bin/python instead of creating top-level source-looking folders.
copy_runtime_py_dir "$XR_ROOT_PROJECT/xr_client" "$XR_OUT_BIN_ROOT/python/xr_client"
copy_tools_runtime
copy_capture_client_runtime
copy_runtime_scripts
prepare_python_runtime
write_app_launchers

package_vendor_components_enabled() {
  case "${XR_PACKAGE_VENDOR_COMPONENTS:-${XR_BUILD_VENDOR_COMPONENTS:-1}}" in
    1|true|TRUE|yes|YES|on|ON) return 0 ;;
    *) return 1 ;;
  esac
}

remove_disabled_vendor_outputs() {
  local relative
  for relative in ${XR_VENDOR_PACKAGE_PATHS:-}; do
    case "$relative" in
      ""|/*|*".."*) fatal "unsafe XR_VENDOR_PACKAGE_PATHS entry: $relative" ;;
    esac
    if [[ -e "$XR_OUT_ROOT/$relative" || -L "$XR_OUT_ROOT/$relative" ]]; then
      log "remove disabled/stale vendor output: $relative"
      rm -rf -- "$XR_OUT_ROOT/$relative"
    fi
  done
}

if [[ -n "${XR_DEVICE_PACKAGE_HOOK:-}" ]]; then
  if package_vendor_components_enabled; then
    if [[ ! -x "$XR_DEVICE_PACKAGE_HOOK" ]]; then
      fatal "vendor package hook is not executable: $XR_DEVICE_PACKAGE_HOOK"
    fi
    log "run vendor package hook: $XR_DEVICE_PACKAGE_HOOK"
    "$XR_DEVICE_PACKAGE_HOOK"
  else
    log "skip vendor package hook: XR_PACKAGE_VENDOR_COMPONENTS=${XR_PACKAGE_VENDOR_COMPONENTS:-0}"
    remove_disabled_vendor_outputs
  fi
elif ! package_vendor_components_enabled; then
  remove_disabled_vendor_outputs
fi

# Optional driver runtime metadata/resources. Do not copy driver source trees.
if [[ "$XR_PACKAGE_COPY_DRIVER_RUNTIME" == "1" ]]; then
  # OpenVR driver packages are frequency/mode-specific. The built package content
  # is copied above from XR_PACKAGE_SOURCE_BIN_ROOT; here we add only the runtime
  # registration helper next to each existing variant. Avoid recreating the old
  # generic bin/drivers/openvr_driver package because SteamVR should register
  # exactly one selected variant.
  shopt -s nullglob
  openvr_variant_dirs=("$XR_OUT_BIN_ROOT"/drivers/openvr_driver_*HZ*)
  shopt -u nullglob
  for variant_dir in "${openvr_variant_dirs[@]}"; do
    if [[ -d "$variant_dir/xr_tracking" ]]; then
      copy_runtime_file \
        "$XR_ROOT_PROJECT/drivers/openvr_driver/scripts/register_driver.sh" \
        "$variant_dir/scripts/register_driver.sh"
    fi
  done
  rm -rf "$XR_OUT_BIN_ROOT/drivers/openvr_driver"

  copy_runtime_file "$XR_ROOT_PROJECT/drivers/monado_driver/scripts/linux/start.sh" "$XR_OUT_BIN_ROOT/drivers/monado_driver/start.sh"
fi

# Optional full calibration tree. The target device bundle already contains its selected
# calibration/config subset; copy the full tree only when requested.
if [[ "$XR_PACKAGE_COPY_CALIBRATION_DATASET" == "1" ]]; then
  copy_if_exists "$XR_ROOT_PROJECT/calibration_dataset" "$XR_OUT_ROOT/calibration_dataset"
fi

# The old Python/GStreamer capture_service was removed from the core package.
# Runtime Python now consists of standalone capture_client, xr_client and tools.

find "$XR_OUT_ROOT" -type d -name '__pycache__' -prune -exec rm -rf {} +
find "$XR_OUT_ROOT" -type f \( -name '*.pyc' -o -name '*.pyo' \) -delete

cat > "$XR_OUT_ROOT/run_xr_client.sh" <<'EOF_RUNNER'
#!/usr/bin/env bash
set -euo pipefail
PACKAGE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PY_RUNTIME_ENV="$PACKAGE_ROOT/bin/python-runtime/env.sh"
if [[ ! -f "$PY_RUNTIME_ENV" ]]; then
  echo "[run_xr_client][ERROR] package Python runtime env not found: $PY_RUNTIME_ENV" >&2
  echo "[run_xr_client][ERROR] Rebuild the XR Gate package." >&2
  exit 1
fi
# shellcheck source=/dev/null
source "$PY_RUNTIME_ENV"
export XR_ROOT_PROJECT="${XR_ROOT_PROJECT:-$PACKAGE_ROOT}"
export ROOT_PROJECT="${ROOT_PROJECT:-$XR_ROOT_PROJECT}"
export XR_COMMON_SCRIPTS_ROOT="${XR_COMMON_SCRIPTS_ROOT:-$PACKAGE_ROOT/devices/common/linux/scripts}"
export XR_CLIENT_CONFIG_DIR="${XR_CLIENT_CONFIG_DIR:-$XR_PYTHON_ROOT/xr_client/configs}"
if [[ ! -x "$PYTHON" ]]; then
  echo "[run_xr_client][ERROR] package Python not found: $PYTHON" >&2
  exit 1
fi
if [[ $# -eq 0 ]]; then
  echo "Usage: ./run_xr_client.sh --config <profile> [xr_client options]" >&2
  echo "Examples:" >&2
  echo "  ./run_xr_client.sh --config xreal_ultra" >&2
  echo "  ./run_xr_client.sh --config xreal_ultra --no-imu" >&2
  echo "  ./run_xr_client.sh --config leap_motion_uvc_nrf54l15" >&2
  echo "  ./run_xr_client.sh --config leap_motion_uvc" >&2
  echo "Available profiles:" >&2
  find "$XR_CLIENT_CONFIG_DIR" -maxdepth 1 -type f -name '*.json' -printf '  %f\n' 2>/dev/null | sort >&2 || true
  exit 2
fi
exec "$PYTHON" "$XR_PYTHON_ROOT/xr_client/xr_backend_client.py" "$@"
EOF_RUNNER
chmod +x "$XR_OUT_ROOT/run_xr_client.sh"

cat > "$XR_OUT_ROOT/README_RUN.md" <<'EOF2'
# XR Gate Linux runtime package

This package contains hardware-neutral runtime components and multiple device/
tracking profiles. Select the active stack explicitly through the xr_client
config; the package itself is not tied to XREAL Ultra.

Examples:

```bash
./run_xr_client.sh --config xreal_ultra
./run_xr_client.sh --config xreal_ultra --no-imu
./run_xr_client.sh --config leap_motion_uvc_nrf54l15
./run_xr_client.sh --config leap_motion_uvc
```

Config names are resolved from `bin/python/xr_client/configs`; an explicit JSON
path is also accepted. Device and tracking environments referenced by a config
are loaded from `devices/<profile>/`.

Included runtime profiles are controlled at package-build time with
`XR_PACKAGE_PROFILES`; advanced builds may override `XR_PACKAGE_DEVICE_TARGETS`
and `XR_PACKAGE_CONFIG_PROFILES` separately. Pure vendor binaries/helpers are
built by default and can be disabled with:

```bash
XR_BUILD_VENDOR_COMPONENTS=0 \
  ./devices/common/linux/scripts/build/install_xr_gate_out.sh
```

The generic dependency installer is:

```bash
./devices/common/linux/scripts/runtime/install_runtime_deps_ubuntu24.sh
```

Mercury hand-tracking models remain a separately distributed optional artifact.
Install them into `bin/hand-tracking-models/mercury`.
EOF2

if package_include_xrizer_helpers; then
  cat >> "$XR_OUT_ROOT/README_RUN.md" <<'EOF2'

Optional xrizer launchers are included because the xrizer package is present:

```bash
./run_xrizer_register.sh
./run_xrizer_openvr_app_via_monado.sh --print-steam-options
./run_xrizer_collect_logs.sh
```

xrizer remains licensed under GPL-3.0-or-later. Its license and the exact
Corresponding Source used for this binary are included at:

```text
LICENSES/GPL-3.0-or-later.txt
SOURCES/xrizer/SOURCE.txt
SOURCES/xrizer/xrizer-corresponding-source-<commit>.tar.gz
```
EOF2
fi


# The deploy root should stay runtime-only and flat. Service-specific launch
# material belongs under devices/<target> or bin/scripts; Python runtime code
# belongs under bin/python. Remove stale directories from older package layouts.
for stale_dir in \
  "$XR_OUT_ROOT/backends" \
  "$XR_OUT_ROOT/bridges" \
  "$XR_OUT_ROOT/capture_service" \
  "$XR_OUT_ROOT/drivers" \
  "$XR_OUT_ROOT/override_controller" \
  "$XR_OUT_ROOT/runtime_adapters" \
  "$XR_OUT_ROOT/tools" \
  "$XR_OUT_ROOT/xr_client" \
  "$XR_OUT_ROOT/apps"; do
  rm -rf "$stale_dir"
done

# Quick package sanity checks.
required=(
  "$XR_OUT_ROOT/run_xr_client.sh"
  "$XR_OUT_ROOT/devices/common/common.env"
  "$XR_OUT_ROOT/devices/common/linux/scripts/capture_service/start_capture_service.sh"
  "$XR_OUT_BIN_ROOT/python/xr_client/xr_backend_client.py"
  "$XR_OUT_BIN_ROOT/capture_service_cpp/capture_service_cpp"
  "$XR_OUT_BIN_ROOT/capture_service_cpp/capture_tcp_probe"
  "$XR_OUT_BIN_ROOT/scripts/capture_service_cpp/start_capture_service_cpp.sh"
  "$XR_OUT_BIN_ROOT/python/capture_client/client.py"
  "$XR_OUT_BIN_ROOT/python-runtime/env.sh"
  "$XR_OUT_BIN_ROOT/python-runtime/venv/bin/python"
  "$XR_OUT_ROOT/LICENSE"
  "$XR_OUT_ROOT/LICENSES/MIT.txt"
  "$XR_OUT_ROOT/THIRD_PARTY_NOTICES.md"
  "$XR_OUT_BIN_ROOT"
)
for target in ${XR_PACKAGE_DEVICE_TARGETS:-$XR_TARGET_DEVICE}; do
  required+=("$XR_OUT_ROOT/devices/$target")
done
for profile in ${XR_PACKAGE_CONFIG_PROFILES:-${XR_PACKAGE_PROFILES:-}}; do
  required+=("$XR_OUT_BIN_ROOT/python/xr_client/configs/$profile.json")
done
if [[ -d "$XR_OUT_BIN_ROOT/drivers/xrizer" ]]; then
  required+=(
    "$XR_OUT_ROOT/LICENSES/GPL-3.0-or-later.txt"
    "$XR_OUT_ROOT/SOURCES/xrizer/SOURCE.txt"
    "$XR_OUT_ROOT/SOURCES/xrizer/SHA256SUMS.txt"
  )
  packaged_xrizer_source="$(find "$XR_OUT_ROOT/SOURCES/xrizer" -maxdepth 1 -type f \
    -name 'xrizer-corresponding-source-*.tar.gz' -print -quit)"
  [[ -n "$packaged_xrizer_source" && -f "$packaged_xrizer_source" ]] || \
    fatal "packaged xrizer Corresponding Source archive is missing"
fi
for p in "${required[@]}"; do
  if [[ ! -e "$p" ]]; then
    if [[ "$XR_PACKAGE_ALLOW_PARTIAL" == "1" ]]; then
      log "partial package: required path missing for full runtime: $p"
    else
      fatal "packaged required path missing: $p"
    fi
  fi
done

log "Package ready: $XR_OUT_ROOT"
find "$XR_OUT_ROOT" -maxdepth 4 -type f \( -name '*.sh' -o -perm -111 \) | sed "s#^#[package_device_out:${XR_TARGET_DEVICE}] file #"
