#!/usr/bin/env bash
set -euo pipefail

expand_tilde() {
  local value="$1"
  case "$value" in
    "~") printf '%s\n' "$HOME" ;;
    "~/"*) printf '%s\n' "$HOME/${value#"~/"}" ;;
    *) printf '%s\n' "$value" ;;
  esac
}

find_executable_in_build() {
  local name="$1"
  local candidate="$BASALT_BUILD_DIR/$name"

  if [[ -x "$candidate" ]]; then
    printf '%s\n' "$candidate"
    return 0
  fi

  find "$BASALT_BUILD_DIR" -type f -name "$name" -perm -111 -print -quit 2>/dev/null || true
}

# -----------------------------------------------------------------------------
# Roots / layout
# -----------------------------------------------------------------------------
ROOT_PROJECT="${ROOT_PROJECT:-$HOME/src/xr_tracking}"
ROOT_PROJECT="$(expand_tilde "$ROOT_PROJECT")"

BASALT_DIR="${BASALT_DIR:-$ROOT_PROJECT/third_party/basalt}"
BASALT_DIR="$(expand_tilde "$BASALT_DIR")"

BACKEND_DIR="${BACKEND_DIR:-$ROOT_PROJECT/backends/basalt_vio}"
BACKEND_DIR="$(expand_tilde "$BACKEND_DIR")"

SHARED_INCLUDE_DIR="${SHARED_INCLUDE_DIR:-$ROOT_PROJECT/shared/include}"
SHARED_INCLUDE_DIR="$(expand_tilde "$SHARED_INCLUDE_DIR")"

# Basalt upstream revision used as project integration baseline.
BASALT_REF="${BASALT_REF:-0f3b2b52c807f70ff4e2973ce253c73329eea7bc}"
BASALT_REPO_URL="${BASALT_REPO_URL:-https://github.com/VladyslavUsenko/basalt.git}"

# Patch is optional now. The default flow integrates Basalt idempotently from the
# script, because static patches against upstream CMakeLists.txt/vcpkg.json are
# fragile across Basalt revisions.
APPLY_PATCH="${APPLY_PATCH:-0}"
BASALT_PATCH="${BASALT_PATCH:-$BACKEND_DIR/patches/basalt/basalt_vio_integration.patch}"
BASALT_PATCH="$(expand_tilde "$BASALT_PATCH")"

CMAKE_PRESET="${CMAKE_PRESET:-relwithdebinfo}"
XR_CMAKE_PRESET="${XR_CMAKE_PRESET:-xr_${CMAKE_PRESET}}"

# Build output is project-owned and intentionally outside third_party/basalt.
BASALT_BUILD_ROOT="${BASALT_BUILD_ROOT:-$ROOT_PROJECT/build/backends/basalt_vio}"
BASALT_BUILD_ROOT="$(expand_tilde "$BASALT_BUILD_ROOT")"
BASALT_BUILD_DIR="${BASALT_BUILD_DIR:-$BASALT_BUILD_ROOT/$CMAKE_PRESET}"
BASALT_BUILD_DIR="$(expand_tilde "$BASALT_BUILD_DIR")"

# Final runnable binaries are copied here. This is project /bin, not system /bin.
INSTALL_BIN_DIR="${INSTALL_BIN_DIR:-$ROOT_PROJECT/bin/backends/basalt_vio}"
INSTALL_BIN_DIR="$(expand_tilde "$INSTALL_BIN_DIR")"

# Runtime shared libraries copied next to the installed binaries.
# capture_basalt_backend links against libbasalt.so, so relying on the
# Basalt build tree or a globally installed library is fragile after repo moves.
INSTALL_LIB_DIR="${INSTALL_LIB_DIR:-$INSTALL_BIN_DIR/lib}"
INSTALL_LIB_DIR="$(expand_tilde "$INSTALL_LIB_DIR")"

# Optional final config dump path.
XR_CALIB_DIR="${XR_CALIB_DIR:-$ROOT_PROJECT/calibration_dataset}"
XR_CALIB_DIR="$(expand_tilde "$XR_CALIB_DIR")"
XR_DEVICE_NAME="${XR_DEVICE_NAME:-xreal_air2ultra}"
XR_SERIAL="${XR_SERIAL:-ZBBM5DZFMP}"
CALIB_PROFILE_NAME="${CALIB_PROFILE_NAME:-unified_480_ccw90}"
FINAL_PROFILE_DIR="${FINAL_PROFILE_DIR:-$XR_CALIB_DIR/final/$XR_DEVICE_NAME/$XR_SERIAL/$CALIB_PROFILE_NAME}"
FINAL_PROFILE_DIR="$(expand_tilde "$FINAL_PROFILE_DIR")"
BASALT_VIO_CONFIG_OUTPUT="${BASALT_VIO_CONFIG_OUTPUT:-$FINAL_PROFILE_DIR/basalt_vio_config_unified_480_ccw90.json}"
BASALT_VIO_CONFIG_OUTPUT="$(expand_tilde "$BASALT_VIO_CONFIG_OUTPUT")"

INSTALL_APT_DEPS="${INSTALL_APT_DEPS:-1}"
CLONE_BASALT="${CLONE_BASALT:-1}"
FETCH_BASALT="${FETCH_BASALT:-1}"
UPDATE_SUBMODULES="${UPDATE_SUBMODULES:-1}"
ALLOW_DIRTY_BASALT="${ALLOW_DIRTY_BASALT:-0}"
CLEAN_OLD_OVERLAY="${CLEAN_OLD_OVERLAY:-1}"
CLEAN_BUILD="${CLEAN_BUILD:-0}"
BUILD_TARGETS="${BUILD_TARGETS:-capture_basalt_backend xr_dump_vio_config}"
DUMP_VIO_CONFIG="${DUMP_VIO_CONFIG:-1}"

if [[ -x /snap/bin/cmake ]]; then
  CMAKE_BIN="${CMAKE_BIN:-/snap/bin/cmake}"
else
  CMAKE_BIN="${CMAKE_BIN:-cmake}"
fi

# -----------------------------------------------------------------------------
# Sanity checks
# -----------------------------------------------------------------------------
echo "ROOT_PROJECT=$ROOT_PROJECT"
echo "BASALT_DIR=$BASALT_DIR"
echo "BACKEND_DIR=$BACKEND_DIR"
echo "SHARED_INCLUDE_DIR=$SHARED_INCLUDE_DIR"
echo "BASALT_REPO_URL=$BASALT_REPO_URL"
echo "BASALT_REF=$BASALT_REF"
echo "APPLY_PATCH=$APPLY_PATCH"
echo "BASALT_PATCH=$BASALT_PATCH"
echo "CMAKE_BIN=$CMAKE_BIN"
echo "CMAKE_PRESET=$CMAKE_PRESET"
echo "XR_CMAKE_PRESET=$XR_CMAKE_PRESET"
echo "BASALT_BUILD_DIR=$BASALT_BUILD_DIR"
echo "INSTALL_BIN_DIR=$INSTALL_BIN_DIR"
echo "INSTALL_LIB_DIR=$INSTALL_LIB_DIR"
echo "BUILD_TARGETS=$BUILD_TARGETS"
echo

if [[ ! -d "$ROOT_PROJECT" ]]; then
  echo "[ERROR] ROOT_PROJECT does not exist: $ROOT_PROJECT" >&2
  exit 1
fi

if [[ ! -f "$BACKEND_DIR/CMakeLists.txt" ]]; then
  echo "[ERROR] basalt_vio backend layout not found: $BACKEND_DIR/CMakeLists.txt" >&2
  echo "[ERROR] Expected: $ROOT_PROJECT/backends/basalt_vio/CMakeLists.txt" >&2
  exit 1
fi

if [[ ! -d "$SHARED_INCLUDE_DIR/capture_client" ]]; then
  echo "[ERROR] shared capture_client headers not found: $SHARED_INCLUDE_DIR/capture_client" >&2
  exit 1
fi

if [[ "$APPLY_PATCH" == "1" && ! -f "$BASALT_PATCH" ]]; then
  echo "[ERROR] Basalt integration patch not found: $BASALT_PATCH" >&2
  echo "[ERROR] Expected under: $BACKEND_DIR/patches/basalt/" >&2
  exit 1
fi

# -----------------------------------------------------------------------------
# Host dependencies
# -----------------------------------------------------------------------------
if [[ "$INSTALL_APT_DEPS" == "1" ]]; then
  sudo apt update
  sudo apt install -y \
    ninja-build \
    build-essential \
    pkg-config \
    git \
    curl \
    zip unzip tar \
    python3 python3-tk \
    autoconf automake autoconf-archive \
    libtool \
    bison flex \
    nasm \
    libglu1-mesa-dev \
    mesa-common-dev \
    libgl1-mesa-dev
fi

# -----------------------------------------------------------------------------
# Basalt source
# -----------------------------------------------------------------------------
ensure_basalt_repo() {
  if [[ -d "$BASALT_DIR/.git" ]]; then
    echo "[OK] Basalt repo already exists: $BASALT_DIR"

    if [[ "$FETCH_BASALT" == "1" ]]; then
      echo "[INFO] Fetching Basalt refs/tags"
      git -C "$BASALT_DIR" fetch --tags origin
    fi

    local checkout_ref="$BASALT_REF"

    if ! git -C "$BASALT_DIR" rev-parse --verify --quiet "${checkout_ref}^{commit}" >/dev/null; then
      if git -C "$BASALT_DIR" rev-parse --verify --quiet "origin/${BASALT_REF}^{commit}" >/dev/null; then
        checkout_ref="origin/${BASALT_REF}"
      fi
    fi

    if ! git -C "$BASALT_DIR" rev-parse --verify --quiet "${checkout_ref}^{commit}" >/dev/null; then
      echo "[ERROR] Basalt ref/commit not found: $BASALT_REF" >&2
      echo "[ERROR] Repo: $BASALT_DIR" >&2
      exit 1
    fi

    local target_commit
    local current_commit
    target_commit="$(git -C "$BASALT_DIR" rev-parse "${checkout_ref}^{commit}")"
    current_commit="$(git -C "$BASALT_DIR" rev-parse HEAD)"

    if [[ "$current_commit" != "$target_commit" ]]; then
      if [[ -n "$(git -C "$BASALT_DIR" status --porcelain)" && "$ALLOW_DIRTY_BASALT" != "1" ]]; then
        echo "[ERROR] Basalt repo is dirty and needs checkout to: $BASALT_REF" >&2
        echo "[ERROR] Repo: $BASALT_DIR" >&2
        git -C "$BASALT_DIR" status --short >&2
        echo "[ERROR] Commit/stash/reset changes first, or run with ALLOW_DIRTY_BASALT=1." >&2
        exit 1
      fi

      echo "[INFO] Checking out Basalt ref: $checkout_ref"
      git -C "$BASALT_DIR" checkout "$checkout_ref"
    else
      echo "[OK] Basalt already at requested ref: $BASALT_REF"
    fi
  else
    if [[ "$CLONE_BASALT" != "1" ]]; then
      echo "[ERROR] Basalt repo not found: $BASALT_DIR" >&2
      echo "[ERROR] Clone it first:" >&2
      echo "  git clone '$BASALT_REPO_URL' '$BASALT_DIR'" >&2
      echo "  cd '$BASALT_DIR' && git checkout '$BASALT_REF'" >&2
      echo >&2
      echo "[ERROR] Or run with CLONE_BASALT=1." >&2
      exit 1
    fi

    echo "[INFO] Cloning Basalt into: $BASALT_DIR"
    mkdir -p "$(dirname "$BASALT_DIR")"
    git clone "$BASALT_REPO_URL" "$BASALT_DIR"
    git -C "$BASALT_DIR" checkout "$BASALT_REF"
  fi

  if [[ "$UPDATE_SUBMODULES" == "1" ]]; then
    echo "[INFO] Updating Basalt submodules"
    git -C "$BASALT_DIR" submodule update --init --recursive
  fi
}

ensure_basalt_repo
cd "$BASALT_DIR"

# Remove files that used to be rsynced into third_party/basalt by the old flat
# overlay flow. The new flow builds from xr_tracking/backends/basalt_vio.
if [[ "$CLEAN_OLD_OVERLAY" == "1" ]]; then
  rm -f "$BASALT_DIR/src/capture_basalt_backend.cpp"
  rm -f "$BASALT_DIR/tools/xr/xr_dump_vio_config.cpp"
  rmdir "$BASALT_DIR/tools/xr" 2>/dev/null || true
  rm -f "$BASALT_DIR/include/basalt/tracking/pose_shm_publisher.hpp"
  rmdir "$BASALT_DIR/include/basalt/tracking" 2>/dev/null || true
fi

# -----------------------------------------------------------------------------
# Basalt integration
# -----------------------------------------------------------------------------
remove_realsense2_from_vcpkg() {
  if [[ ! -f "$BASALT_DIR/vcpkg.json" ]]; then
    return 0
  fi

  python3 - "$BASALT_DIR/vcpkg.json" <<'PY'
import json
import sys
from pathlib import Path

path = Path(sys.argv[1])
data = json.loads(path.read_text())
deps = data.get("dependencies", [])

def dep_name(dep):
    if isinstance(dep, str):
        return dep
    if isinstance(dep, dict):
        return dep.get("name")
    return None

new_deps = [dep for dep in deps if dep_name(dep) != "realsense2"]

if new_deps != deps:
    data["dependencies"] = new_deps
    path.write_text(json.dumps(data, indent=2) + "\n")
    print("[OK] removed realsense2 from", path)
else:
    print("[OK] realsense2 not present in", path)
PY
}

ensure_basalt_external_backend_integration() {
  python3 - "$BASALT_DIR/CMakeLists.txt" <<'PY'
import sys
from pathlib import Path

path = Path(sys.argv[1])
s = path.read_text()

old = """add_executable(capture_basalt_backend src/capture_basalt_backend.cpp)
target_include_directories(capture_basalt_backend PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(capture_basalt_backend PRIVATE basalt nlohmann_json::nlohmann_json CLI11::CLI11)
"""

changed = False
if old in s:
    s = s.replace(old, "")
    changed = True
    print("[OK] removed old flat capture_basalt_backend integration")

if "XR_TRACKING_ROOT" not in s:
    s += """

# ---- XR Tracking external backends ----
set(XR_TRACKING_ROOT "" CACHE PATH "Path to xr_tracking repository")

if(XR_TRACKING_ROOT)
  message(STATUS "XR_TRACKING_ROOT=${XR_TRACKING_ROOT}")

  add_subdirectory(
    "${XR_TRACKING_ROOT}/backends/basalt_vio"
    "${CMAKE_BINARY_DIR}/xr_tracking/backends/basalt_vio"
  )
endif()
"""
    changed = True
    print("[OK] added XR_TRACKING_ROOT external backend integration")
else:
    print("[OK] XR_TRACKING_ROOT integration already present")

if changed:
    path.write_text(s)
PY
}

if [[ "$APPLY_PATCH" == "1" ]]; then
  if git apply --reverse --check "$BASALT_PATCH" >/dev/null 2>&1; then
    echo "[OK] Basalt integration patch already applied: $BASALT_PATCH"
  else
    git apply --check "$BASALT_PATCH"
    git apply "$BASALT_PATCH"
    echo "[OK] Basalt integration patch applied: $BASALT_PATCH"
  fi
else
  remove_realsense2_from_vcpkg
  ensure_basalt_external_backend_integration
fi

echo
echo "[git status]"
git status --short

# -----------------------------------------------------------------------------
# Configure and build
# -----------------------------------------------------------------------------
export XR_TRACKING_ROOT="$ROOT_PROJECT"
mkdir -p "$BASALT_BUILD_DIR" "$INSTALL_BIN_DIR" "$INSTALL_LIB_DIR"

if [[ "$CLEAN_BUILD" == "1" ]]; then
  rm -rf "$BASALT_BUILD_DIR"
  mkdir -p "$BASALT_BUILD_DIR"
fi

# Generate a CMakeUserPresets.json inside the Basalt checkout that inherits the
# upstream preset but redirects binaryDir to xr_tracking/build/...
python3 - "$BASALT_DIR/CMakeUserPresets.json" "$XR_CMAKE_PRESET" "$CMAKE_PRESET" "$BASALT_BUILD_DIR" "$ROOT_PROJECT" <<'PY'
import json
import sys
from pathlib import Path

path = Path(sys.argv[1])
name = sys.argv[2]
inherits = sys.argv[3]
binary_dir = sys.argv[4]
root = sys.argv[5]

payload = {
    "version": 3,
    "configurePresets": [
        {
            "name": name,
            "inherits": inherits,
            "binaryDir": binary_dir,
            "cacheVariables": {
                "XR_TRACKING_ROOT": root,
            },
        }
    ],
}

path.write_text(json.dumps(payload, indent=2) + "\n")
print("[OK] wrote", path)
PY

echo
echo "[cmake version]"
"$CMAKE_BIN" --version

echo
echo "[cmake configure]"
cd "$BASALT_DIR"
"$CMAKE_BIN" --preset "$XR_CMAKE_PRESET"

echo
echo "[build targets]"
for target in $BUILD_TARGETS; do
  "$CMAKE_BIN" --build "$BASALT_BUILD_DIR" --target "$target" -j"$(nproc)"
done

echo
echo "[copy runtime libraries]"
mapfile -t basalt_runtime_libs < <(find "$BASALT_BUILD_DIR" -type f -name 'libbasalt.so*' -print 2>/dev/null | sort)
if (( ${#basalt_runtime_libs[@]} == 0 )); then
  echo "[WARN] libbasalt.so not found under build dir: $BASALT_BUILD_DIR" >&2
  echo "[WARN] Installed binaries may require BASALT_LIB_DIR/LD_LIBRARY_PATH at runtime." >&2
else
  for lib in "${basalt_runtime_libs[@]}"; do
    cp -a "$lib" "$INSTALL_LIB_DIR/"
    ls -lh "$INSTALL_LIB_DIR/$(basename "$lib")"
  done
fi

# Make just-installed helper tools runnable during this install step even when
# the current shell does not already know where libbasalt.so lives.
export LD_LIBRARY_PATH="$INSTALL_LIB_DIR:${LD_LIBRARY_PATH:-}"

echo
echo "[copy binaries]"
for target in $BUILD_TARGETS; do
  built_bin="$(find_executable_in_build "$target")"
  if [[ -z "$built_bin" ]]; then
    echo "[WARN] Built binary not found for target: $target" >&2
    continue
  fi

  tmp_bin="$INSTALL_BIN_DIR/$target.new.$$"
  cp "$built_bin" "$tmp_bin"
  chmod +x "$tmp_bin"
  mv -f "$tmp_bin" "$INSTALL_BIN_DIR/$target"
  ls -lh "$INSTALL_BIN_DIR/$target"
done

echo
echo "[built files]"
for target in $BUILD_TARGETS; do
  find "$BASALT_BUILD_DIR" -maxdepth 5 -type f -name "$target" -print 2>/dev/null || true
done

# -----------------------------------------------------------------------------
# Optional: dump default Basalt VIO config into the active calibration profile
# -----------------------------------------------------------------------------
if [[ "$DUMP_VIO_CONFIG" == "1" ]]; then
  DUMP_BIN="$INSTALL_BIN_DIR/xr_dump_vio_config"

  if [[ ! -x "$DUMP_BIN" ]]; then
    FOUND_DUMP_BIN="$(find_executable_in_build xr_dump_vio_config)"
    if [[ -n "$FOUND_DUMP_BIN" ]]; then
      DUMP_BIN="$FOUND_DUMP_BIN"
    fi
  fi

  if [[ ! -x "$DUMP_BIN" ]]; then
    echo "[WARN] xr_dump_vio_config binary not found; skipping config dump" >&2
  else
    mkdir -p "$(dirname "$BASALT_VIO_CONFIG_OUTPUT")"
    "$DUMP_BIN" "$BASALT_VIO_CONFIG_OUTPUT"
    echo "[OK] wrote Basalt VIO config: $BASALT_VIO_CONFIG_OUTPUT"
  fi
fi

echo
echo "[OK] Basalt VIO backend installed/built"
echo "Build dir: $BASALT_BUILD_DIR"
echo "Run binary: $INSTALL_BIN_DIR/capture_basalt_backend"
