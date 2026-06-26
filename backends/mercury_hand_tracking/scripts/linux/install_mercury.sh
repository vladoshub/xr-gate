#!/usr/bin/env bash
set -euo pipefail

# Build XR Mercury hand-tracking backend from project-owned files in xr_tracking.
# Third-party source trees are expected under $ROOT_PROJECT/third_party.
# Build outputs go under $ROOT_PROJECT/build/backends/mercury_hand_tracking.
# User-facing binaries/libs are copied to $ROOT_PROJECT/bin/backends/mercury_hand_tracking.

expand_tilde() {
  local value="$1"
  case "$value" in
    "~") printf '%s\n' "$HOME" ;;
    "~/"*) printf '%s\n' "$HOME/${value#"~/"}" ;;
    *) printf '%s\n' "$value" ;;
  esac
}

log() {
  printf '\n\033[1;32m[install_mercury]\033[0m %s\n' "$*" >&2
}

warn() {
  printf '\n\033[1;33m[install_mercury][WARN]\033[0m %s\n' "$*" >&2
}

fail() {
  printf '\n\033[1;31m[install_mercury][ERROR]\033[0m %s\n' "$*" >&2
  exit 1
}

require_file() {
  local p="$1"
  [[ -f "$p" ]] || fail "required file not found: $p"
}

require_dir() {
  local p="$1"
  [[ -d "$p" ]] || fail "required directory not found: $p"
}

# Project layout.
ROOT_PROJECT="${ROOT_PROJECT:-$HOME/src/xr_tracking}"
ROOT_PROJECT="$(expand_tilde "$ROOT_PROJECT")"
BUILD_JOBS="${BUILD_JOBS:-$(nproc 2>/dev/null || echo 2)}"

BACKEND_DIR="${BACKEND_DIR:-$ROOT_PROJECT/backends/mercury_hand_tracking}"
THIRD_PARTY_DIR="${THIRD_PARTY_DIR:-$ROOT_PROJECT/third_party}"
MONADO_DIR="${MONADO_DIR:-$THIRD_PARTY_DIR/monado}"
MERCURY_DRIVER_DIR="${MERCURY_DRIVER_DIR:-$THIRD_PARTY_DIR/mercury_steamvr_driver}"
PROJECT_OVERLAY="${PROJECT_OVERLAY:-$BACKEND_DIR/monado_overlay}"
MONADO_PATCH="${MONADO_PATCH:-$BACKEND_DIR/patches/monado/mercury_xr_upstream_changes.patch}"

BACKEND_DIR="$(expand_tilde "$BACKEND_DIR")"
THIRD_PARTY_DIR="$(expand_tilde "$THIRD_PARTY_DIR")"
MONADO_DIR="$(expand_tilde "$MONADO_DIR")"
MERCURY_DRIVER_DIR="$(expand_tilde "$MERCURY_DRIVER_DIR")"
PROJECT_OVERLAY="$(expand_tilde "$PROJECT_OVERLAY")"
MONADO_PATCH="$(expand_tilde "$MONADO_PATCH")"

# Third-party source refs.
MONADO_REPO_URL="${MONADO_REPO_URL:-https://gitlab.freedesktop.org/monado/monado.git}"
MONADO_COMMIT="${MONADO_COMMIT:-6c9804934324ac5a3e68d64938579dbbeb4d75b3}"
MONADO_BRANCH="${MONADO_BRANCH:-xr-mercury-base-6c98049}"
CLONE_MONADO="${CLONE_MONADO:-1}"
FETCH_MONADO="${FETCH_MONADO:-1}"
RESET_MONADO_TREE="${RESET_MONADO_TREE:-1}"

MERCURY_DRIVER_REPO_URL="${MERCURY_DRIVER_REPO_URL:-https://github.com/moshimeow/mercury_steamvr_driver.git}"
MERCURY_DRIVER_REF="${MERCURY_DRIVER_REF:-e3948ace94a9f2cbd949adf50ffcc082002337cc}"
CLONE_MERCURY_DRIVER="${CLONE_MERCURY_DRIVER:-1}"
FETCH_MERCURY_DRIVER="${FETCH_MERCURY_DRIVER:-1}"
RESET_MERCURY_DRIVER_TREE="${RESET_MERCURY_DRIVER_TREE:-1}"

# Dependencies/build outputs.
INSTALL_APT_DEPS="${INSTALL_APT_DEPS:-1}"
INSTALL_LFS_DEPS="${INSTALL_LFS_DEPS:-1}"
CMAKE_BUILD_TYPE="${CMAKE_BUILD_TYPE:-RelWithDebInfo}"

BUILD_ROOT="${BUILD_ROOT:-$ROOT_PROJECT/build/backends/mercury_hand_tracking}"
MONADO_BUILD_DIR="${MONADO_BUILD_DIR:-$BUILD_ROOT/monado_xr_mercury}"
BACKEND_BUILD_DIR="${BACKEND_BUILD_DIR:-$BUILD_ROOT/backend}"
INSTALL_BIN_DIR="${INSTALL_BIN_DIR:-$ROOT_PROJECT/bin/backends/mercury_hand_tracking}"

BUILD_ROOT="$(expand_tilde "$BUILD_ROOT")"
MONADO_BUILD_DIR="$(expand_tilde "$MONADO_BUILD_DIR")"
BACKEND_BUILD_DIR="$(expand_tilde "$BACKEND_BUILD_DIR")"
INSTALL_BIN_DIR="$(expand_tilde "$INSTALL_BIN_DIR")"

# ONNXRuntime.
ORT_VERSION="${ORT_VERSION:-1.18.1}"
ORT_ROOT="${ORT_ROOT:-$ROOT_PROJECT/bin/onnxruntime/onnxruntime-linux-x64-$ORT_VERSION}"
ORT_ARCHIVE="${ORT_ARCHIVE:-$ROOT_PROJECT/bin/onnxruntime/onnxruntime-linux-x64-${ORT_VERSION}.tgz}"
ORT_URL="${ORT_URL:-https://github.com/microsoft/onnxruntime/releases/download/v${ORT_VERSION}/onnxruntime-linux-x64-${ORT_VERSION}.tgz}"
ORT_ROOT="$(expand_tilde "$ORT_ROOT")"
ORT_ARCHIVE="$(expand_tilde "$ORT_ARCHIVE")"
MERCURY_MODELS="${MERCURY_MODELS:-$ROOT_PROJECT/bin/hand-tracking-models/mercury}"
MERCURY_MODELS="$(expand_tilde "$MERCURY_MODELS")"
# Model files are optional/downstream assets. Do not fetch/copy them by default
# during backend build; enable explicitly or run download_mercury_models.sh.
INSTALL_MERCURY_MODELS="${INSTALL_MERCURY_MODELS:-0}"

ensure_git_repo() {
  local name="$1"
  local dir="$2"
  local url="$3"
  local ref="$4"
  local clone_flag="$5"
  local fetch_flag="$6"

  if [[ -d "$dir/.git" ]]; then
    log "$name repo already exists: $dir"
  else
    if [[ "$clone_flag" != "1" ]]; then
      fail "$name repo not found: $dir
Clone it first, for example:
  git clone '$url' '$dir'
  cd '$dir' && git checkout '$ref'
Or run this script with the matching clone flag enabled."
    fi

    log "Cloning $name into: $dir"
    mkdir -p "$(dirname "$dir")"
    git clone "$url" "$dir"
  fi

  if [[ "$fetch_flag" == "1" ]]; then
    log "Fetching $name refs/tags"
    git -C "$dir" fetch --tags origin
  fi

  local checkout_ref="$ref"
  if ! git -C "$dir" rev-parse --verify --quiet "${checkout_ref}^{commit}" >/dev/null; then
    if git -C "$dir" rev-parse --verify --quiet "origin/${ref}^{commit}" >/dev/null; then
      checkout_ref="origin/${ref}"
    fi
  fi

  if ! git -C "$dir" rev-parse --verify --quiet "${checkout_ref}^{commit}" >/dev/null; then
    fail "$name ref/commit not found: $ref in $dir"
  fi

  echo "$checkout_ref"
}

log "Project paths"
echo "ROOT_PROJECT=$ROOT_PROJECT"
echo "BACKEND_DIR=$BACKEND_DIR"
echo "THIRD_PARTY_DIR=$THIRD_PARTY_DIR"
echo "MONADO_DIR=$MONADO_DIR"
echo "MERCURY_DRIVER_DIR=$MERCURY_DRIVER_DIR"
echo "BUILD_ROOT=$BUILD_ROOT"
echo "MONADO_BUILD_DIR=$MONADO_BUILD_DIR"
echo "BACKEND_BUILD_DIR=$BACKEND_BUILD_DIR"
echo "INSTALL_BIN_DIR=$INSTALL_BIN_DIR"

require_dir "$BACKEND_DIR"
require_dir "$PROJECT_OVERLAY"
require_file "$PROJECT_OVERLAY/src/xrt/tracking/hand/mercury/xr_mercury_runtime_c_api.cpp"
require_file "$PROJECT_OVERLAY/src/xrt/tracking/hand/mercury/xr_mercury_runtime_c_api.h"
require_file "$PROJECT_OVERLAY/src/xrt/tracking/hand/mercury/xr_mercury_dataset_probe.cpp"
require_file "$MONADO_PATCH"

if [[ "$INSTALL_APT_DEPS" == "1" ]]; then
  log "Installing apt dependencies"
  sudo apt update
  sudo apt install -y \
    git cmake ninja-build build-essential pkg-config \
    curl tar unzip rsync python3 python3-pip \
    libopencv-dev libeigen3-dev libceres-dev \
    libcjson-dev libjson-c-dev \
    libudev-dev libhidapi-dev libusb-1.0-0-dev \
    libvulkan-dev glslang-tools \
    libx11-dev libxrandr-dev libxxf86vm-dev libxcb-randr0-dev libx11-xcb-dev \
    libwayland-dev libxinerama-dev \
    libgl1-mesa-dev libegl1-mesa-dev \
    nlohmann-json3-dev libcli11-dev
fi

if [[ "$INSTALL_LFS_DEPS" == "1" ]]; then
  log "Installing git-lfs dependency"
  sudo apt update
  sudo apt install -y git-lfs
fi

log "Installing ONNXRuntime $ORT_VERSION under $ORT_ROOT"
mkdir -p "$(dirname "$ORT_ARCHIVE")" "$(dirname "$ORT_ROOT")"
if [[ ! -d "$ORT_ROOT" ]]; then
  curl -L -o "$ORT_ARCHIVE" "$ORT_URL"
  tar -xzf "$ORT_ARCHIVE" -C "$(dirname "$ORT_ROOT")"
fi
require_file "$ORT_ROOT/include/onnxruntime_c_api.h"
require_file "$ORT_ROOT/lib/libonnxruntime.so"
ls -lh "$ORT_ROOT/include/onnxruntime_c_api.h"
ls -lh "$ORT_ROOT/lib"/libonnxruntime.so*

mkdir -p "$THIRD_PARTY_DIR" "$BUILD_ROOT" "$INSTALL_BIN_DIR"

log "Ensuring Monado source tree"
MONADO_CHECKOUT_REF="$(ensure_git_repo "Monado" "$MONADO_DIR" "$MONADO_REPO_URL" "$MONADO_COMMIT" "$CLONE_MONADO" "$FETCH_MONADO")"

cd "$MONADO_DIR"
if [[ "$RESET_MONADO_TREE" == "1" ]]; then
  log "Resetting Monado to clean base commit: $MONADO_COMMIT"
  git checkout -B "$MONADO_BRANCH" "$MONADO_CHECKOUT_REF"
  git reset --hard "$MONADO_COMMIT"
  git clean -fd
else
  git checkout "$MONADO_CHECKOUT_REF"
fi

git submodule sync --recursive
git submodule update --init --recursive

echo "Monado commit:"
git rev-parse HEAD

log "Copying project-owned XR Mercury files from $PROJECT_OVERLAY"
rsync -av "$PROJECT_OVERLAY/" "$MONADO_DIR/"

require_file "$MONADO_DIR/src/xrt/tracking/hand/mercury/xr_mercury_runtime_c_api.cpp"
require_file "$MONADO_DIR/src/xrt/tracking/hand/mercury/xr_mercury_runtime_c_api.h"
require_file "$MONADO_DIR/src/xrt/tracking/hand/mercury/xr_mercury_dataset_probe.cpp"

log "Applying upstream Monado/Mercury patch"
cd "$MONADO_DIR"
if git apply --reverse --check "$MONADO_PATCH" >/dev/null 2>&1; then
  echo "[OK] mercury_xr_upstream_changes.patch already applied"
else
  git apply --check "$MONADO_PATCH"
  git apply "$MONADO_PATCH"
  echo "[OK] mercury_xr_upstream_changes.patch applied"
fi

log "Checking patched CMakeLists"
grep -n "xr_mercury_runtime\|xr_mercury_dataset_probe" \
  "$MONADO_DIR/src/xrt/tracking/hand/mercury/CMakeLists.txt" || \
  fail "Mercury CMakeLists.txt does not contain xr targets after patch"

git status --short

log "Configuring Monado/Mercury with ONNXRuntime enabled"
rm -rf "$MONADO_BUILD_DIR"
export LD_LIBRARY_PATH="$ORT_ROOT/lib:${LD_LIBRARY_PATH:-}"

cmake -S "$MONADO_DIR" -B "$MONADO_BUILD_DIR" -G Ninja \
  -DCMAKE_BUILD_TYPE="$CMAKE_BUILD_TYPE" \
  -DONNXRuntime_INCLUDE_DIR="$ORT_ROOT/include" \
  -DONNXRuntime_LIBRARY="$ORT_ROOT/lib/libonnxruntime.so" \
  -DMODULE_MERCURY_HANDTRACKING=ON

log "Checking CMake ONNX/Mercury state"
grep -E 'ONNXRUNTIME|MODULE_MERCURY_HANDTRACKING|ONNXRuntime_INCLUDE_DIR|ONNXRuntime_LIBRARY' \
  "$MONADO_BUILD_DIR/CMakeCache.txt" || true

if ! ninja -C "$MONADO_BUILD_DIR" -t targets | grep -q '^xr_mercury_runtime:'; then
  echo
  echo "=== Diagnostic: targets containing xr/mercury/hand ==="
  ninja -C "$MONADO_BUILD_DIR" -t targets | grep -Ei 'xr|mercury|hand' | head -160 || true
  echo
  echo "=== Diagnostic: CMakeCache ONNX/Mercury ==="
  grep -E 'ONNXRUNTIME|MODULE_MERCURY_HANDTRACKING|ONNXRuntime_INCLUDE_DIR|ONNXRuntime_LIBRARY' \
    "$MONADO_BUILD_DIR/CMakeCache.txt" || true
  fail "Ninja target xr_mercury_runtime was not generated"
fi

log "Building Mercury runtime and dataset probe"
cmake --build "$MONADO_BUILD_DIR" \
  --target xr_mercury_runtime xr_mercury_dataset_probe \
  -j"$BUILD_JOBS"

RUNTIME_SO="$MONADO_BUILD_DIR/src/xrt/tracking/hand/mercury/libxr_mercury_runtime.so"
DATASET_PROBE="$MONADO_BUILD_DIR/src/xrt/tracking/hand/mercury/xr_mercury_dataset_probe"
require_file "$RUNTIME_SO"
require_file "$DATASET_PROBE"

log "Checking shared-library dependencies"
ldd "$RUNTIME_SO" | grep -Ei 'not found|onnx|opencv|ceres' || true

log "Building XR Mercury hand-tracking backend"
rm -rf "$BACKEND_BUILD_DIR"
cmake -S "$BACKEND_DIR" -B "$BACKEND_BUILD_DIR" -G Ninja \
  -DCMAKE_BUILD_TYPE="$CMAKE_BUILD_TYPE" \
  -DXR_ROOT="$ROOT_PROJECT"

cmake --build "$BACKEND_BUILD_DIR" \
  --target capture_hand_tracking_backend \
  -j"$BUILD_JOBS"

BACKEND_BIN="$BACKEND_BUILD_DIR/capture_hand_tracking_backend"
require_file "$BACKEND_BIN"

log "Installing/copying runtime artifacts to $INSTALL_BIN_DIR"
mkdir -p "$INSTALL_BIN_DIR"
cp "$BACKEND_BIN" "$INSTALL_BIN_DIR/capture_hand_tracking_backend"
cp "$RUNTIME_SO" "$INSTALL_BIN_DIR/libxr_mercury_runtime.so"
cp "$DATASET_PROBE" "$INSTALL_BIN_DIR/xr_mercury_dataset_probe"
chmod +x "$INSTALL_BIN_DIR/capture_hand_tracking_backend" "$INSTALL_BIN_DIR/xr_mercury_dataset_probe"
ls -lh "$INSTALL_BIN_DIR"

if [[ "$INSTALL_MERCURY_MODELS" == "1" ]]; then
  log "Downloading/copying Mercury hand-tracking models to $MERCURY_MODELS"
  env \
    ROOT_PROJECT="$ROOT_PROJECT" \
    THIRD_PARTY_DIR="$THIRD_PARTY_DIR" \
    MERCURY_DRIVER_DIR="$MERCURY_DRIVER_DIR" \
    MERCURY_DRIVER_REPO_URL="$MERCURY_DRIVER_REPO_URL" \
    MERCURY_DRIVER_REF="$MERCURY_DRIVER_REF" \
    CLONE_MERCURY_DRIVER="$CLONE_MERCURY_DRIVER" \
    FETCH_MERCURY_DRIVER="$FETCH_MERCURY_DRIVER" \
    RESET_MERCURY_DRIVER_TREE="$RESET_MERCURY_DRIVER_TREE" \
    INSTALL_LFS_DEPS="$INSTALL_LFS_DEPS" \
    MERCURY_MODELS="$MERCURY_MODELS" \
    bash "$BACKEND_DIR/scripts/linux/download_mercury_models.sh"
else
  log "Skipping Mercury model download/copy: INSTALL_MERCURY_MODELS=$INSTALL_MERCURY_MODELS"
  echo "To install models later, run:"
  echo "  MERCURY_MODELS='$MERCURY_MODELS' bash '$BACKEND_DIR/scripts/linux/download_mercury_models.sh'"
fi

cat <<MSG

[OK] Mercury hand-tracking build completed.

Build directories:
  Monado/Mercury: $MONADO_BUILD_DIR
  Backend:        $BACKEND_BUILD_DIR

Installed runtime artifacts:
  $INSTALL_BIN_DIR/capture_hand_tracking_backend
  $INSTALL_BIN_DIR/libxr_mercury_runtime.so
  $INSTALL_BIN_DIR/xr_mercury_dataset_probe

For current shell runtime tests, use:
  export LD_LIBRARY_PATH="$ORT_ROOT/lib:\${LD_LIBRARY_PATH:-}"
MSG
