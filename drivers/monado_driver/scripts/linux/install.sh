#!/usr/bin/env bash
set -euo pipefail

# Full installer for the project-owned XR Tracking Monado runtime driver.
# It clones/updates Monado, patches the local Monado checkout, builds it, and
# places the launch binaries/wrappers under $ROOT_PROJECT/bin/drivers/monado_driver.

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
BUILD_JOBS="${BUILD_JOBS:-$(nproc 2>/dev/null || echo 2)}"

expand_path() {
  local path="$1"
  if [[ "$path" == "~" ]]; then
    printf '%s\n' "$HOME"
  elif [[ "$path" == "~/"* ]]; then
    printf '%s/%s\n' "$HOME" "${path#~/}"
  else
    printf '%s\n' "$path"
  fi
}

is_truthy() {
  case "${1:-}" in
    1|true|TRUE|yes|YES|on|ON) return 0 ;;
    *) return 1 ;;
  esac
}

log() { echo "[install_monado_driver] $*"; }
fail() { echo "[install_monado_driver][ERROR] $*" >&2; exit 1; }

need_cmd() {
  command -v "$1" >/dev/null 2>&1 || fail "required command not found: $1"
}

# Resolve project root robustly from wherever this script lives.
# Normal source path is drivers/monado_driver/scripts/linux/install.sh,
# but this also survives moving/copying scripts during local testing.
default_root_from_script() {
  local dir="$SCRIPT_DIR"

  while [[ "$dir" != "/" && -n "$dir" ]]; do
    if [[ -d "$dir/shared/include" && -d "$dir/drivers/monado_driver" ]]; then
      printf '%s\n' "$dir"
      return 0
    fi
    dir="$(dirname "$dir")"
  done

  # Fallback for the expected source-tree location:
  #   drivers/monado_driver/scripts/linux -> project root
  cd "$SCRIPT_DIR/../../../.." >/dev/null 2>&1 && pwd
}

ROOT_PROJECT="$(expand_path "${ROOT_PROJECT:-$(default_root_from_script)}")"
THIRD_PARTY_DIR="$(expand_path "${THIRD_PARTY_DIR:-$ROOT_PROJECT/third_party}")"
MONADO_DIR="$(expand_path "${MONADO_DIR:-$THIRD_PARTY_DIR/monado_driver}")"
DRIVER_DIR="$(expand_path "${DRIVER_DIR:-$ROOT_PROJECT/drivers/monado_driver}")"
PATCH_DIR="$(expand_path "${PATCH_DIR:-$DRIVER_DIR/patches}")"
DST_DIR="$MONADO_DIR/src/xrt/drivers/xr_tracking_runtime"
BUILD_TYPE="${BUILD_TYPE:-RelWithDebInfo}"
BUILD_DIR="$(expand_path "${BUILD_DIR:-$MONADO_DIR/build/xr_tracking_relwithdebinfo}")"
BIN_DIR="$(expand_path "${BIN_DIR:-$ROOT_PROJECT/bin/drivers/monado_driver}")"

# Device target for packaging/wrapper scripts. The Monado runtime driver itself
# consumes generic runtime streams, but the packaged launch/display helpers live
# under devices/<target>/... and may be device-specific.
XR_MONADO_DEVICE_RAW="${XR_MONADO_DEVICE:-${XR_MONADO_DRIVER_DEVICE:-${XR_DEVICE_TARGET:-${XR_TARGET_DEVICE:-xreal_ultra}}}}"
case "${XR_MONADO_DEVICE_RAW}" in
  xreal_ultra|xreal_air2ultra)
    XR_MONADO_DEVICE="xreal_ultra"
    ;;
  *)
    fail "Unsupported XR_MONADO_DEVICE=${XR_MONADO_DEVICE_RAW}; supported: xreal_ultra"
    ;;
esac

# Runtime package helper scripts are installed under the device tree inside the
# package root, for example:
#   out/xreal_ultra/devices/xreal_ultra/linux/scripts/monado_driver/
# When XR_BIN_ROOT points at out/xreal_ultra/bin, infer package root from it.
default_package_root() {
  if [[ -n "${XR_PACKAGE_ROOT:-}" ]]; then
    expand_path "$XR_PACKAGE_ROOT"
    return 0
  fi

  if [[ -n "${XR_BIN_ROOT:-}" ]]; then
    dirname -- "$(expand_path "$XR_BIN_ROOT")"
    return 0
  fi

  case "$BIN_DIR" in
    */bin/drivers/monado_driver)
      printf '%s\n' "${BIN_DIR%/bin/drivers/monado_driver}"
      ;;
    *)
      printf '%s\n' "$ROOT_PROJECT"
      ;;
  esac
}

PACKAGE_ROOT="$(expand_path "${PACKAGE_ROOT:-$(default_package_root)}")"
DEVICE_MONADO_SCRIPT_SRC_DIR="$(expand_path "${DEVICE_MONADO_SCRIPT_SRC_DIR:-$ROOT_PROJECT/devices/$XR_MONADO_DEVICE/linux/scripts/monado_driver}")"
DEVICE_MONADO_SCRIPT_OUT_DIR="$(expand_path "${DEVICE_MONADO_SCRIPT_OUT_DIR:-$PACKAGE_ROOT/devices/$XR_MONADO_DEVICE/linux/scripts/monado_driver}")"

MONADO_REPO="${MONADO_REPO:-https://gitlab.freedesktop.org/monado/monado.git}"
MONADO_REF="${MONADO_REF:-7363fee94b66671efdce79655b8b143d7c9eeecd}"
CLONE_MONADO="${CLONE_MONADO:-1}"
FETCH_MONADO="${FETCH_MONADO:-0}"
UPDATE_SUBMODULES="${UPDATE_SUBMODULES:-1}"
INSTALL_APT_DEPS="${INSTALL_APT_DEPS:-1}"
BUILD_MONADO="${BUILD_MONADO:-1}"
XR_MONADO_ENABLE_X11="${XR_MONADO_ENABLE_X11:-1}"
MONADO_EXTRA_CMAKE_ARGS="${MONADO_EXTRA_CMAKE_ARGS:-}"
CMAKE_GENERATOR="${CMAKE_GENERATOR:-}"
CLEAN_BUILD="${CLEAN_BUILD:-0}"
CLEAN_BUILD_ON_GENERATOR_MISMATCH="${CLEAN_BUILD_ON_GENERATOR_MISMATCH:-1}"
# Some Monado revisions can expose stale Rift constellation-tracking code in
# target_builder_rift.c. It is unrelated to the XR Tracking runtime driver,
# but it can break CI/package builds against pinned Monado refs.
PATCH_RIFT_CONSTELLATION_COMPILE="${PATCH_RIFT_CONSTELLATION_COMPILE:-1}"
# Keep the Monado checkout deterministic before applying project patches.
# Without this, an old already-patched checkout can make the marker-based
# patch application skip a newer version of the same integration patch.
RESET_MONADO_CHECKOUT="${RESET_MONADO_CHECKOUT:-1}"

PROJECT_COMPILE_PATCH="$PATCH_DIR/project/0028-monado-driver-compile-fixes.patch"
MONADO_INTEGRATION_PATCH="$PATCH_DIR/monado/0001-add-xr-tracking-runtime-driver.patch"

maybe_install_apt_deps() {
  if ! is_truthy "$INSTALL_APT_DEPS"; then
    log "skipping apt dependencies because INSTALL_APT_DEPS=$INSTALL_APT_DEPS"
    return 0
  fi
  if ! command -v apt-get >/dev/null 2>&1; then
    log "apt-get not found; skipping apt dependencies"
    return 0
  fi

  local sudo_cmd=()
  if [[ "${EUID:-$(id -u)}" -ne 0 ]]; then
    need_cmd sudo
    sudo_cmd=(sudo)
  fi

  log "installing baseline build/runtime dependencies via apt"
  "${sudo_cmd[@]}" apt-get update
  "${sudo_cmd[@]}" apt-get install -y \
    git rsync ca-certificates \
    cmake ninja-build build-essential pkg-config python3 \
    libvulkan-dev vulkan-tools glslang-tools \
    libx11-dev libxrandr-dev libxext-dev libxfixes-dev libxxf86vm-dev \
    libxcb1-dev libxcb-randr0-dev libxcb-present-dev libxcb-dri3-dev libxcb-xfixes0-dev libxcb-keysyms1-dev \
    libudev-dev libhidapi-dev libwayland-dev wayland-protocols libxkbcommon-dev \
    libeigen3-dev libopencv-dev libssl-dev libsystemd-dev libdbus-1-dev
}

ensure_monado_checkout() {
  need_cmd git

  if [[ -d "$MONADO_DIR/src/xrt" ]]; then
    log "using existing Monado source tree: $MONADO_DIR"
  else
    if [[ -e "$MONADO_DIR" && ! -d "$MONADO_DIR/.git" ]]; then
      fail "path exists but is not a Monado source tree: $MONADO_DIR"
    fi
    if ! is_truthy "$CLONE_MONADO"; then
      fail "Monado source tree not found: $MONADO_DIR. Set CLONE_MONADO=1 or clone it manually."
    fi

    mkdir -p "$(dirname "$MONADO_DIR")"
    log "cloning Monado into: $MONADO_DIR"
    log "repo: $MONADO_REPO"
    if [[ -n "$MONADO_REF" ]]; then
      log "will checkout ref after clone: $MONADO_REF"
    fi

    # Do not use 'git clone --branch' here: MONADO_REF may be a raw commit SHA,
    # while --branch only works reliably for branch/tag names.
    git clone "$MONADO_REPO" "$MONADO_DIR"
  fi

  if [[ -d "$MONADO_DIR/.git" ]]; then
    if is_truthy "$FETCH_MONADO"; then
      log "fetching Monado updates"
      git -C "$MONADO_DIR" fetch --tags --prune origin
    fi

    if [[ -n "$MONADO_REF" ]]; then
      log "checking out Monado ref: $MONADO_REF"

      if ! git -C "$MONADO_DIR" rev-parse --verify --quiet "$MONADO_REF^{commit}" >/dev/null; then
        log "ref is not available locally; fetching origin metadata"
        git -C "$MONADO_DIR" fetch --tags --prune origin
      fi

      if git -C "$MONADO_DIR" rev-parse --verify --quiet "$MONADO_REF^{commit}" >/dev/null; then
        git -C "$MONADO_DIR" checkout --detach "$MONADO_REF"
      else
        # Last fallback for branch names, tag names, or servers allowing direct SHA fetches.
        git -C "$MONADO_DIR" fetch origin "$MONADO_REF"
        git -C "$MONADO_DIR" checkout --detach FETCH_HEAD
      fi

      if is_truthy "$RESET_MONADO_CHECKOUT"; then
        log "resetting Monado checkout before applying project patches"
        git -C "$MONADO_DIR" reset --hard HEAD
      else
        log "not resetting Monado checkout because RESET_MONADO_CHECKOUT=$RESET_MONADO_CHECKOUT"
      fi
    fi

    if is_truthy "$UPDATE_SUBMODULES"; then
      log "updating Monado submodules"
      git -C "$MONADO_DIR" submodule update --init --recursive
    fi
  fi

  [[ -d "$MONADO_DIR/src/xrt" ]] || fail "Monado checkout does not contain src/xrt: $MONADO_DIR"
}

apply_git_patch_once() {
  local repo_dir="$1"
  local patch_file="$2"
  local marker_file="$3"
  local marker_regex="$4"
  local label="$5"

  [[ -f "$patch_file" ]] || fail "missing patch file: $patch_file"
  [[ -f "$repo_dir/$marker_file" ]] || fail "missing marker file: $repo_dir/$marker_file"

  if grep -Eq "$marker_regex" "$repo_dir/$marker_file"; then
    log "$label already present; skipping patch"
    return 0
  fi

  log "applying $label"
  if git -C "$repo_dir" apply --check "$patch_file" >/dev/null 2>&1; then
    git -C "$repo_dir" apply "$patch_file"
  else
    fail "cannot apply $label: $patch_file. Check repo state or apply manually."
  fi
}

patch_driver_cmake_and_headers() {
  local base_dir="$1"
  local cmake_file="$base_dir/CMakeLists.txt"
  local header_file="$base_dir/include/xr_monado_driver/runtime_readers.hpp"

  [[ -f "$cmake_file" ]] || fail "driver CMakeLists.txt not found: $cmake_file"
  [[ -f "$header_file" ]] || fail "runtime_readers.hpp not found: $header_file"

  python3 - "$cmake_file" "$header_file" <<'PY'
from pathlib import Path
import re
import sys

cmake = Path(sys.argv[1])
header = Path(sys.argv[2])

cmake.write_text(r'''# Intended to be included from inside a Monado checkout, after Monado has set up
# include paths for xrt/ and util/. This is not a standalone project.

add_library(drv_xr_tracking_runtime STATIC
  src/runtime_readers.cpp
  src/posix_runtime_readers.cpp
  src/windows_runtime_readers.cpp
  src/xr_monado_driver.cpp
)

if(NOT DEFINED XR_TRACKING_ROOT)
  if(DEFINED ENV{XR_TRACKING_ROOT})
    set(XR_TRACKING_ROOT "$ENV{XR_TRACKING_ROOT}")
  else()
    # Case A: project-owned source at drivers/monado_driver.
    get_filename_component(_xr_tracking_root_from_project "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
    # Case B: copied source inside third_party/monado_driver/src/xrt/drivers/xr_tracking_runtime.
    get_filename_component(_xr_tracking_root_from_monado_copy "${CMAKE_CURRENT_SOURCE_DIR}/../../../../../.." ABSOLUTE)

    if(EXISTS "${_xr_tracking_root_from_project}/shared/include/xr_runtime/contracts/runtime_pose_stream.hpp")
      set(XR_TRACKING_ROOT "${_xr_tracking_root_from_project}")
    elseif(EXISTS "${_xr_tracking_root_from_monado_copy}/shared/include/xr_runtime/contracts/runtime_pose_stream.hpp")
      set(XR_TRACKING_ROOT "${_xr_tracking_root_from_monado_copy}")
    endif()
  endif()
endif()

set(_xr_tracking_shared_include "${XR_TRACKING_ROOT}/shared/include")
set(_xr_tracking_runtime_pose_header "${_xr_tracking_shared_include}/xr_runtime/contracts/runtime_pose_stream.hpp")
set(_xr_tracking_controller_state_header "${_xr_tracking_shared_include}/xr_runtime/contracts/runtime_controller_state_contract.hpp")
set(_xr_tracking_legacy_hand_header "${_xr_tracking_shared_include}/xr_tracking/publishers/hand_tracking_shm_publisher.hpp")

if(NOT DEFINED XR_TRACKING_ROOT
   OR NOT EXISTS "${_xr_tracking_runtime_pose_header}"
   OR NOT EXISTS "${_xr_tracking_controller_state_header}"
   OR NOT EXISTS "${_xr_tracking_legacy_hand_header}")
  message(FATAL_ERROR
    "XR_TRACKING_ROOT is not set correctly; expected shared headers under ${XR_TRACKING_ROOT}/shared/include. "
    "Missing one of: ${_xr_tracking_runtime_pose_header}, ${_xr_tracking_controller_state_header}, ${_xr_tracking_legacy_hand_header}")
endif()

target_include_directories(drv_xr_tracking_runtime PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${_xr_tracking_shared_include}
)

target_compile_features(drv_xr_tracking_runtime PRIVATE cxx_std_17)
target_link_libraries(drv_xr_tracking_runtime PRIVATE xrt-interfaces aux_util)

if(UNIX AND NOT APPLE)
  target_link_libraries(drv_xr_tracking_runtime PRIVATE rt)
endif()
''')

s = header.read_text()
s = s.replace('#include <xr_tracking/contracts/hand_tracking_contract.hpp>',
              '#include <xr_tracking/publishers/hand_tracking_shm_publisher.hpp>')
header.write_text(s)
PY
}

patch_identity_distortion() {
  local file="$1/src/xr_monado_driver.cpp"
  [[ -f "$file" ]] || fail "xr_monado_driver.cpp not found: $file"

  python3 - "$file" <<'PY'
from pathlib import Path
import sys

p = Path(sys.argv[1])
s = p.read_text()
orig = s

if '#include "util/u_distortion_mesh.h"' not in s:
    marker = '#include "util/u_device.h"\n'
    if marker not in s:
        raise SystemExit(f"marker not found in {p}: {marker!r}")
    s = s.replace(marker, marker + '#include "util/u_distortion_mesh.h"\n', 1)

if 'u_distortion_mesh_set_none(xdev);' not in s:
    marker = '  u_device_setup_split_side_by_side(xdev, &info);\n'
    if marker not in s:
        raise SystemExit(f"marker not found in {p}: {marker!r}")
    s = s.replace(marker, marker + '\n  // MVP passthrough profile: no lens distortion.\n  u_distortion_mesh_set_none(xdev);\n', 1)

if s != orig:
    p.write_text(s)
PY
}

patch_project_driver_sources() {
  [[ -d "$DRIVER_DIR" ]] || fail "monado_driver source dir not found: $DRIVER_DIR"

  # Apply the branch/API compile patch only when still on the first draft source.
  if [[ -f "$PROJECT_COMPILE_PATCH" ]]; then
    if grep -q 'XRT_DEVICE_WMR_CONTROLLER' "$DRIVER_DIR/src/xr_monado_driver.cpp" \
       || grep -q 'XRT_INPUT_WMR_GRIP_POSE' "$DRIVER_DIR/src/xr_monado_driver.cpp"; then
      log "project driver compile patch already present; skipping"
    else
      apply_git_patch_once "$ROOT_PROJECT" "$PROJECT_COMPILE_PATCH" \
        "drivers/monado_driver/src/xr_monado_driver.cpp" \
        "XRT_INPUT_WMR_GRIP_POSE|XRT_DEVICE_WMR_CONTROLLER" \
        "project driver compile fixes"
    fi
  else
    log "warning: project compile patch not found: $PROJECT_COMPILE_PATCH"
  fi

  patch_driver_cmake_and_headers "$DRIVER_DIR"
  patch_identity_distortion "$DRIVER_DIR"
}

copy_project_driver_to_monado() {
  need_cmd rsync
  mkdir -p "$DST_DIR"
  rsync -a --delete \
    --exclude 'patches' \
    --exclude 'scripts' \
    "$DRIVER_DIR/" "$DST_DIR/"
  patch_driver_cmake_and_headers "$DST_DIR"
  patch_identity_distortion "$DST_DIR"
  log "copied project-owned driver sources to: $DST_DIR"
}

apply_monado_integration_patch() {
  apply_git_patch_once "$MONADO_DIR" "$MONADO_INTEGRATION_PATCH" \
    "src/xrt/targets/common/target_builder_legacy.c" \
    "XR_TRACKING_RUNTIME_ENABLE|xr_tracking_runtime_hmd_create" \
    "Monado legacy builder integration"
}

patch_xreal_air_builder_guard() {
  local file="$MONADO_DIR/src/xrt/targets/common/target_builder_xreal_air.c"
  [[ -f "$file" ]] || { log "xreal_air builder not found; skipping guard: $file"; return 0; }

  python3 - "$file" <<'PY'
from pathlib import Path
import sys

path = Path(sys.argv[1])
s = path.read_text()
orig = s

needle = 'DEBUG_GET_ONCE_LOG_OPTION(xreal_air_log, "XREAL_AIR_LOG", U_LOGGING_WARN)\n'
insert = '''DEBUG_GET_ONCE_LOG_OPTION(xreal_air_log, "XREAL_AIR_LOG", U_LOGGING_WARN)\n\n// Project XR runtime driver uses already-published runtime SHM streams.\n// When it is enabled, do not let Monado's built-in xreal_air USB builder\n// claim the glasses first.\nDEBUG_GET_ONCE_BOOL_OPTION(xr_tracking_runtime_enabled_for_xreal_air, "XR_TRACKING_RUNTIME_ENABLE", false)\n'''
if 'xr_tracking_runtime_enabled_for_xreal_air' not in s:
    if needle not in s:
        raise SystemExit(f"marker not found for env option in {path}")
    s = s.replace(needle, insert, 1)

needle = '''\tU_ZERO(estimate);\n\n\txret = xrt_prober_lock_list(xp, &xpdevs, &xpdev_count);\n'''
insert = '''\tU_ZERO(estimate);\n\n\tif (debug_get_bool_option_xr_tracking_runtime_enabled_for_xreal_air()) {\n\t\treturn XRT_SUCCESS;\n\t}\n\n\txret = xrt_prober_lock_list(xp, &xpdevs, &xpdev_count);\n'''
if 'debug_get_bool_option_xr_tracking_runtime_enabled_for_xreal_air()) {\n\t\treturn XRT_SUCCESS;' not in s:
    if needle not in s:
        raise SystemExit(f"marker not found for estimate guard in {path}")
    s = s.replace(needle, insert, 1)

needle = '''\tDRV_TRACE_MARKER();\n\n\txreal_air_log_level = debug_get_log_option_xreal_air_log();\n'''
insert = '''\tDRV_TRACE_MARKER();\n\n\tif (debug_get_bool_option_xr_tracking_runtime_enabled_for_xreal_air()) {\n\t\treturn XRT_ERROR_DEVICE_CREATION_FAILED;\n\t}\n\n\txreal_air_log_level = debug_get_log_option_xreal_air_log();\n'''
if 'debug_get_bool_option_xr_tracking_runtime_enabled_for_xreal_air()) {\n\t\treturn XRT_ERROR_DEVICE_CREATION_FAILED;' not in s:
    if needle not in s:
        raise SystemExit(f"marker not found for open guard in {path}")
    s = s.replace(needle, insert, 1)

if s != orig:
    path.write_text(s)
PY
  log "patched xreal_air builder guard"
}


patch_rift_constellation_compile_guard() {
  if ! is_truthy "$PATCH_RIFT_CONSTELLATION_COMPILE"; then
    log "skipping Rift constellation compile guard because PATCH_RIFT_CONSTELLATION_COMPILE=$PATCH_RIFT_CONSTELLATION_COMPILE"
    return 0
  fi

  local file="$MONADO_DIR/src/xrt/targets/common/target_builder_rift.c"
  [[ -f "$file" ]] || { log "Rift builder not found; skipping constellation compile guard: $file"; return 0; }

  python3 - "$file" <<'PY'
from pathlib import Path
import sys

path = Path(sys.argv[1])
s = path.read_text()
orig = s

# Some pinned/upstream Monado snapshots contain add_devices_to_constellation_tracker()
# code that references rb->constellation_tracker even when struct rift_builder no
# longer has that member. XR Tracking does not use the Rift constellation path, so
# make that helper a no-op instead of failing the whole package build.
if 'rb->constellation_tracker' in s and 'XR Tracking package build: Rift constellation tracking disabled' not in s:
    needle = 'add_devices_to_constellation_tracker(struct rift_builder *rb)'
    name_pos = s.find(needle)
    if name_pos < 0:
        raise SystemExit(f"marker not found in {path}: {needle!r}")

    line_start = s.rfind('\n', 0, name_pos) + 1
    prev_line_start = s.rfind('\n', 0, max(0, line_start - 1)) + 1
    func_start = prev_line_start

    brace_pos = s.find('{', name_pos)
    if brace_pos < 0:
        raise SystemExit(f"function opening brace not found in {path}")

    depth = 0
    end_pos = None
    for i in range(brace_pos, len(s)):
        ch = s[i]
        if ch == '{':
            depth += 1
        elif ch == '}':
            depth -= 1
            if depth == 0:
                end_pos = i + 1
                break
    if end_pos is None:
        raise SystemExit(f"function closing brace not found in {path}")

    replacement = (
        "static int\n"
        "add_devices_to_constellation_tracker(struct rift_builder *rb)\n"
        "{\n"
        "\t// XR Tracking package build: Rift constellation tracking disabled.\n"
        "\t// This avoids stale upstream API references to rb->constellation_tracker.\n"
        "\t(void)rb;\n"
        "\treturn 0;\n"
        "}\n"
    )
    s = s[:func_start] + replacement + s[end_pos:]

if s != orig:
    path.write_text(s)
PY
  log "patched Rift constellation compile guard"
}

read_cmake_cache_generator() {
  local cache_file="$BUILD_DIR/CMakeCache.txt"
  [[ -f "$cache_file" ]] || return 0
  grep -E '^CMAKE_GENERATOR:INTERNAL=' "$cache_file" | sed 's/^CMAKE_GENERATOR:INTERNAL=//' || true
}

configure_and_build_monado() {
  if ! is_truthy "$BUILD_MONADO"; then
    log "skipping build because BUILD_MONADO=$BUILD_MONADO"
    return 0
  fi

  if is_truthy "$CLEAN_BUILD"; then
    log "CLEAN_BUILD=$CLEAN_BUILD; removing build dir: $BUILD_DIR"
    rm -rf "$BUILD_DIR"
  fi

  local existing_generator=""
  existing_generator="$(read_cmake_cache_generator)"

  local selected_generator=""
  local force_generator=0

  if [[ -n "$existing_generator" ]]; then
    log "existing CMake generator in build dir: $existing_generator"
  fi

  if [[ -n "$CMAKE_GENERATOR" ]]; then
    selected_generator="$CMAKE_GENERATOR"
    force_generator=1
  elif [[ -n "$existing_generator" ]]; then
    # Important: do not pass -G when the build directory already has a cache.
    # CMake will reuse the existing generator, avoiding Ninja/Makefile mismatch.
    selected_generator="$existing_generator"
    force_generator=0
  elif command -v ninja >/dev/null 2>&1; then
    selected_generator="Ninja"
    force_generator=1
  fi

  if [[ -n "$existing_generator" && -n "$selected_generator" && "$existing_generator" != "$selected_generator" ]]; then
    if is_truthy "$CLEAN_BUILD_ON_GENERATOR_MISMATCH"; then
      log "generator mismatch: existing '$existing_generator' vs requested '$selected_generator'; removing build dir"
      rm -rf "$BUILD_DIR"
      existing_generator=""
      force_generator=1
    else
      fail "generator mismatch: existing '$existing_generator' vs requested '$selected_generator'. Set CLEAN_BUILD=1 or CLEAN_BUILD_ON_GENERATOR_MISMATCH=1."
    fi
  fi

  local cmake_args=(-S "$MONADO_DIR" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE="$BUILD_TYPE" -DXR_TRACKING_ROOT="$ROOT_PROJECT")

  if is_truthy "$XR_MONADO_ENABLE_X11"; then
    log "requesting Monado X11/XCB compositor target"
    cmake_args+=(
      -DXRT_HAVE_XCB=ON
      -DXRT_HAVE_XLIB=ON
      -DXRT_HAVE_XRANDR=ON
      -DXRT_FEATURE_COMPOSITOR_MAIN=ON
      -DXRT_FEATURE_OPENXR=ON
      -DXRT_FEATURE_SERVICE=ON
    )
  fi

  if [[ -n "$MONADO_EXTRA_CMAKE_ARGS" ]]; then
    # shellcheck disable=SC2206
    local extra_cmake_args=( $MONADO_EXTRA_CMAKE_ARGS )
    cmake_args+=("${extra_cmake_args[@]}")
  fi
  if [[ "$force_generator" -eq 1 && -n "$selected_generator" ]]; then
    cmake_args=(-G "$selected_generator" "${cmake_args[@]}")
  fi

  log "configuring Monado: $BUILD_DIR"
  if [[ -n "$selected_generator" ]]; then
    log "CMake generator: $selected_generator"
  fi
  cmake "${cmake_args[@]}"

  log "building Monado"
  cmake --build "$BUILD_DIR" -j"$BUILD_JOBS"
}

install_device_monado_scripts() {
  mkdir -p "$DEVICE_MONADO_SCRIPT_OUT_DIR"

  local script=""
  local installed_any=0
  local scripts=(
    start_monado_driver.sh
    double_display_fix.sh
    main_display_control.sh
  )

  for script in "${scripts[@]}"; do
    if [[ -f "$DEVICE_MONADO_SCRIPT_SRC_DIR/$script" ]]; then
      install -m 0755 "$DEVICE_MONADO_SCRIPT_SRC_DIR/$script" "$DEVICE_MONADO_SCRIPT_OUT_DIR/$script"
      installed_any=1
    else
      log "warning: device Monado helper script not found: $DEVICE_MONADO_SCRIPT_SRC_DIR/$script"
    fi
  done

  if [[ "$installed_any" -eq 1 ]]; then
    log "installed device Monado helper scripts into: $DEVICE_MONADO_SCRIPT_OUT_DIR"
    find "$DEVICE_MONADO_SCRIPT_OUT_DIR" -maxdepth 1 -type f -perm -111 -printf '  %p\n' | sort || true
  fi
}

install_openxr_runtime_manifest() {
  local openxr_lib=""
  local lib_name="libopenxr_monado.so"
  local manifest="$DEVICE_MONADO_SCRIPT_OUT_DIR/openxr_monado_xrgate.json"
  local env_script="$DEVICE_MONADO_SCRIPT_OUT_DIR/openxr_runtime_env.sh"
  local relative_lib="../../../../../bin/drivers/monado_driver/$lib_name"

  openxr_lib="$(find "$BUILD_DIR" -type f \( -name 'libopenxr_monado.so' -o -name 'libopenxr_monado.so.*' \) -print 2>/dev/null | sort | head -n1 || true)"

  if [[ -z "$openxr_lib" ]]; then
    log "warning: libopenxr_monado.so was not found under build dir; XR_RUNTIME_JSON manifest will not be generated"
    return 0
  fi

  mkdir -p "$BIN_DIR" "$DEVICE_MONADO_SCRIPT_OUT_DIR"
  install -m 0755 "$openxr_lib" "$BIN_DIR/$(basename "$openxr_lib")"

  if [[ "$(basename "$openxr_lib")" != "$lib_name" ]]; then
    ln -sfn "$(basename "$openxr_lib")" "$BIN_DIR/$lib_name"
  fi

  cat > "$manifest" <<EOF
{
  "file_format_version": "1.0.0",
  "runtime": {
    "name": "XR Gate Monado",
    "library_path": "$relative_lib"
  }
}
EOF

  cat > "$env_script" <<'EOF'
#!/usr/bin/env bash
# Source this file to force OpenXR applications to use the XR Gate packaged Monado runtime:
#   source devices/xreal_ultra/linux/scripts/monado_driver/openxr_runtime_env.sh
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export XR_RUNTIME_JSON="$SCRIPT_DIR/openxr_monado_xrgate.json"
echo "XR_RUNTIME_JSON=$XR_RUNTIME_JSON"
EOF
  chmod 0755 "$env_script"

  log "installed OpenXR runtime library: $BIN_DIR/$lib_name"
  log "installed OpenXR runtime manifest: $manifest"
  log "installed OpenXR env helper: $env_script"
}

install_runtime_binaries() {
  mkdir -p "$BIN_DIR"

  local service_bin="$BUILD_DIR/src/xrt/targets/service/monado-service"
  [[ -x "$service_bin" ]] || fail "monado-service not found after build: $service_bin"
  install -m 0755 "$service_bin" "$BIN_DIR/monado-service"

  local cli_bin="$BUILD_DIR/src/xrt/targets/cli/monado-cli"
  if [[ -x "$cli_bin" ]]; then
    install -m 0755 "$cli_bin" "$BIN_DIR/monado-cli"
  fi

  # Install a copy of start.sh next to the binary for a stable user-facing path.
  if [[ -f "$SCRIPT_DIR/start.sh" ]]; then
    install -m 0755 "$SCRIPT_DIR/start.sh" "$BIN_DIR/start.sh"
  fi

  install_openxr_runtime_manifest

  cat > "$BIN_DIR/env.sh" <<EOF
# Generated by drivers/monado_driver/scripts/linux/install.sh
export XR_TRACKING_ROOT="$ROOT_PROJECT"
export XR_TRACKING_RUNTIME_ENABLE=1
export XR_MONADO_DEVICE="$XR_MONADO_DEVICE"
export XR_TARGET_DEVICE="${XR_TARGET_DEVICE:-$XR_MONADO_DEVICE}"
export XR_DEVICE_TARGET="${XR_DEVICE_TARGET:-$XR_MONADO_DEVICE}"
export XR_MONADO_SERVICE_BIN="$BIN_DIR/monado-service"
export XR_RUNTIME_JSON="$DEVICE_MONADO_SCRIPT_OUT_DIR/openxr_monado_xrgate.json"
EOF

  log "installed runtime binaries into: $BIN_DIR"
  find "$BIN_DIR" -maxdepth 1 -type f -perm -111 -printf '  %p\n' | sort || true
}

main() {
  log "ROOT_PROJECT=$ROOT_PROJECT"
  log "MONADO_DIR=$MONADO_DIR"
  log "DRIVER_DIR=$DRIVER_DIR"
  log "BUILD_DIR=$BUILD_DIR"
  log "BIN_DIR=$BIN_DIR"
  log "XR_MONADO_DEVICE=$XR_MONADO_DEVICE"
  log "PACKAGE_ROOT=$PACKAGE_ROOT"
  log "DEVICE_MONADO_SCRIPT_SRC_DIR=$DEVICE_MONADO_SCRIPT_SRC_DIR"
  log "DEVICE_MONADO_SCRIPT_OUT_DIR=$DEVICE_MONADO_SCRIPT_OUT_DIR"
  log "MONADO_REF=$MONADO_REF"
  log "RESET_MONADO_CHECKOUT=$RESET_MONADO_CHECKOUT"
  log "XR_MONADO_ENABLE_X11=$XR_MONADO_ENABLE_X11"

  need_cmd cmake
  need_cmd python3
  maybe_install_apt_deps
  ensure_monado_checkout
  patch_project_driver_sources
  copy_project_driver_to_monado
  apply_monado_integration_patch
  if [[ "$XR_MONADO_DEVICE" == "xreal_ultra" ]]; then
    patch_xreal_air_builder_guard
  else
    log "skipping xreal_air builder guard for XR_MONADO_DEVICE=$XR_MONADO_DEVICE"
  fi
  patch_rift_constellation_compile_guard
  configure_and_build_monado
  install_runtime_binaries
  install_device_monado_scripts

  log "done"
  log "start with: $BIN_DIR/start.sh"
  log "device wrapper: $DEVICE_MONADO_SCRIPT_OUT_DIR/start_monado_driver.sh"
  log "display helper: $DEVICE_MONADO_SCRIPT_OUT_DIR/double_display_fix.sh"
  log "display control: $DEVICE_MONADO_SCRIPT_OUT_DIR/main_display_control.sh"
}

main "$@"
