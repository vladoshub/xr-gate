#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=/dev/null
source "$SCRIPT_DIR/xreal_ultra_out_env.sh"

log() { echo "[package_xreal_ultra_out] $*" >&2; }
fatal() { echo "[package_xreal_ultra_out][ERROR] $*" >&2; exit 1; }

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

copy_device_bundle() {
  local src="$XR_ROOT_PROJECT/devices/xreal_ultra"
  local dst="$XR_OUT_DEVICE_HOME"
  if [[ ! -d "$src" ]]; then
    log "skip missing device bundle: $src"
    return 0
  fi
  mkdir -p "$dst"
  rsync -a --delete \
    --exclude='/linux/scripts/install_xreal_ultra_out.sh' \
    --exclude='/linux/scripts/package_xreal_ultra_out.sh' \
    --exclude='/linux/scripts/xreal_ultra_out_env.sh' \
    --exclude='__pycache__/' \
    --exclude='*.pyc' \
    --exclude='*.pyo' \
    "$src/" "$dst/"
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

copy_runtime_scripts() {
  # Only launcher/runtime scripts that are called by devices/xreal_ultra wrappers
  # are copied here. Build/install scripts, CMake files and source trees stay out
  # of the deploy package.
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
    "$XR_ROOT_PROJECT/backends/xr_spatial/scripts/linux/start_xr_spatial_shm.sh" \
    "$XR_OUT_BIN_ROOT/scripts/backends/xr_spatial/start_xr_spatial_shm.sh"
  copy_runtime_file \
    "$XR_ROOT_PROJECT/backends/xr_spatial/scripts/linux/start_xr_spatial_scan.sh" \
    "$XR_OUT_BIN_ROOT/scripts/backends/xr_spatial/start_xr_spatial_scan.sh"
  copy_runtime_file \
    "$XR_ROOT_PROJECT/backends/xr_spatial/scripts/linux/xr_spatial_profile.sh" \
    "$XR_OUT_BIN_ROOT/scripts/backends/xr_spatial/xr_spatial_profile.sh"

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
    "$XR_ROOT_PROJECT/drivers/steam_vr/scripts/linux/restore_xreal_desktop.sh" \
    "$XR_OUT_BIN_ROOT/scripts/drivers/steam_vr/restore_xreal_desktop.sh"
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
  write_root_launcher "run_openvr_dgpu_direct.sh" "bin/scripts/drivers/steam_vr/start_openvr_dgpu_direct.sh"
  write_root_launcher "run_openvr_dgpu_direct_60.sh" "bin/scripts/drivers/steam_vr/start_openvr_dgpu_direct_60.sh"
  write_root_launcher "run_openvr_restore_desktop.sh" "bin/scripts/drivers/steam_vr/restore_xreal_desktop.sh"
  write_root_launcher "download_mercury_models.sh" "devices/xreal_ultra/linux/scripts/mercury_hand_tracking/download_mercury_models.sh"
  write_root_launcher "run_steamvr_video_overlay.sh" "devices/xreal_ultra/linux/scripts/steamvr_video_overlay/start_steamvr_video_overlay.sh"
  write_root_launcher "run_steamvr_spatial_overlay.sh" "devices/xreal_ultra/linux/scripts/steamvr_spatial_overlay/start_xr_steamvr_spatial_overlay.sh"
  write_root_launcher "run_steamvr_spatial_scene.sh" "devices/xreal_ultra/linux/scripts/steamvr_spatial_scene/start_xr_steamvr_spatial_scene.sh"
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

# Device bundle: env, launch wrappers and device-local configs/calibration.
# Package/build entrypoints live in the source device tree, but are excluded from
# the runtime package to keep out/xreal_ultra runtime-only.
copy_device_bundle

# Runtime Python entrypoints. They are required at runtime, so keep them under
# bin/python instead of creating top-level source-looking folders.
copy_runtime_py_dir "$XR_ROOT_PROJECT/xr_client" "$XR_OUT_BIN_ROOT/python/xr_client"
copy_tools_runtime
copy_capture_client_runtime
copy_runtime_scripts
prepare_python_runtime
write_app_launchers

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

# Optional full calibration tree. The device bundle already contains the selected
# XREAL Ultra calibration/config subset; copy the full tree only when requested.
if [[ "$XR_PACKAGE_COPY_CALIBRATION_DATASET" == "1" ]]; then
  copy_if_exists "$XR_ROOT_PROJECT/calibration_dataset" "$XR_OUT_ROOT/calibration_dataset"
fi

# The old Python/GStreamer capture_service was removed from the core package.
# Runtime Python now consists of standalone capture_client, xr_client and tools.

find "$XR_OUT_ROOT" -type d -name '__pycache__' -prune -exec rm -rf {} +
find "$XR_OUT_ROOT" -type f \( -name '*.pyc' -o -name '*.pyo' \) -delete

cat > "$XR_OUT_ROOT/run_xr_client.sh" <<'RUNNER'
#!/usr/bin/env bash
set -euo pipefail
PACKAGE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PY_RUNTIME_ENV="$PACKAGE_ROOT/bin/python-runtime/env.sh"
if [[ ! -f "$PY_RUNTIME_ENV" ]]; then
  echo "[run_xr_client][ERROR] package Python runtime env not found: $PY_RUNTIME_ENV" >&2
  echo "[run_xr_client][ERROR] Rebuild package with devices/xreal_ultra/linux/scripts/package_xreal_ultra_out.sh" >&2
  exit 1
fi
# shellcheck source=/dev/null
source "$PY_RUNTIME_ENV"
export XR_DEVICE_ENV="${XR_DEVICE_ENV:-$PACKAGE_ROOT/devices/xreal_ultra/xreal_ultra.env}"
if [[ ! -x "$PYTHON" ]]; then
  echo "[run_xr_client][ERROR] package Python not found: $PYTHON" >&2
  exit 1
fi
exec "$PYTHON" "$XR_PYTHON_ROOT/xr_client/xr_backend_client.py" \
  --config "$XR_PYTHON_ROOT/xr_client/configs/default_shm.json" "$@"
RUNNER
chmod +x "$XR_OUT_ROOT/run_xr_client.sh"

cat > "$XR_OUT_ROOT/README_RUN.md" <<EOF2
# XREAL Ultra runtime package

This directory is intended to be copied to another Linux machine with matching
system dependencies/drivers installed.

Run:

\`\`\`bash
cd "$XR_OUT_ROOT"
./run_xr_client.sh
\`\`\`

Important roots are configured in:

\`\`\`bash
devices/xreal_ultra/xreal_ultra.env
\`\`\`

Key variables:

- XR_ROOT_PROJECT: package root, defaults to this directory.
- XR_BIN_ROOT: runtime binary root, defaults to \$XR_ROOT_PROJECT/bin.
- XR_DEVICE_HOME: device profile root, defaults to \$XR_ROOT_PROJECT/devices/xreal_ultra.
- XR_DEVICE_SCRIPTS_ROOT: launch wrapper root.
- XR_DEVICE_CONFIGS_ROOT: device configs/calibration root.

The package intentionally does not include C++ source trees, CMake projects or
build scripts. It includes only runtime binaries/libraries/assets, device launch
scripts/configs, and Python modules that are executed at runtime.

Mercury hand-tracking ONNX models are optional and are not downloaded during the
default build. To install them into \`bin/hand-tracking-models/mercury\`, run:

\`\`\`bash
./download_mercury_models.sh
\`\`\`

Python runtime note: this package creates a thin local venv at
\$XR_ROOT_PROJECT/bin/python-runtime/venv and uses project Python files from
\$XR_ROOT_PROJECT/bin/python. The venv is created with --system-site-packages and
intentionally does not bundle heavy native packages. Install GStreamer, PyGObject,
OpenCV, NumPy and HID runtime dependencies through apt on the target system.

Check Python visibility with:

\`\`\`bash
bin/python-runtime/check_python_runtime.sh
\`\`\`

capture_service note: capture_service_cpp is the runtime capture backend in this
package. The old Python/GStreamer capture_service is not included in core builds.

Convenience app launchers from this package root:

\`\`\`bash
./run_steamvr_video_overlay.sh
./run_steamvr_spatial_overlay.sh
./run_steamvr_spatial_scene.sh
\`\`\`
EOF2

if package_include_xrizer_helpers; then
  cat >> "$XR_OUT_ROOT/README_RUN.md" <<'EOF2'

Optional xrizer launchers are included because the xrizer package is present:

\`\`\`bash
./run_xrizer_register.sh
./run_xrizer_openvr_app_via_monado.sh --print-steam-options
./run_xrizer_collect_logs.sh
\`\`\`
EOF2
fi


# The deploy root should stay runtime-only and flat. Service-specific launch
# material belongs under devices/xreal_ultra or bin/scripts; Python runtime code
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
  "$XR_OUT_DEVICE_HOME/xreal_ultra.env"
  "$XR_OUT_BIN_ROOT/python/xr_client/xr_backend_client.py"
  "$XR_OUT_BIN_ROOT/capture_service_cpp/capture_service_cpp"
  "$XR_OUT_BIN_ROOT/python/capture_client/client.py"
  "$XR_OUT_BIN_ROOT/python-runtime/env.sh"
  "$XR_OUT_BIN_ROOT/python-runtime/venv/bin/python"
  "$XR_OUT_BIN_ROOT"
)
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
find "$XR_OUT_ROOT" -maxdepth 4 -type f \( -name '*.sh' -o -perm -111 \) | sed 's#^#[package_xreal_ultra_out] file #'
