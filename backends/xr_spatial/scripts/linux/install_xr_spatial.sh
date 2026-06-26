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

log() { echo "[install_xr_spatial] $*" >&2; }

ROOT_PROJECT="${ROOT_PROJECT:-$HOME/src/xr_tracking}"
BACKEND_DIR="${BACKEND_DIR:-$ROOT_PROJECT/backends/xr_spatial}"
CMAKE_BUILD_TYPE="${CMAKE_BUILD_TYPE:-RelWithDebInfo}"
BUILD_NAME="${BUILD_NAME:-relwithdebinfo}"
BUILD_ROOT="${BUILD_ROOT:-$ROOT_PROJECT/build/backends/xr_spatial}"
BUILD_DIR="${BUILD_DIR:-$BUILD_ROOT/$BUILD_NAME}"
INSTALL_BIN_DIR="${INSTALL_BIN_DIR:-$ROOT_PROJECT/bin/backends/xr_spatial}"

INSTALL_RUNTIME_BUNDLE="${INSTALL_RUNTIME_BUNDLE:-1}"
INSTALL_BUNDLE_CONFIGS="${INSTALL_BUNDLE_CONFIGS:-1}"
INSTALL_BUNDLE_SCRIPTS="${INSTALL_BUNDLE_SCRIPTS:-1}"
INSTALL_BUNDLE_CALIBRATION="${INSTALL_BUNDLE_CALIBRATION:-1}"
CHECK_INSTALLED_LDD="${CHECK_INSTALLED_LDD:-1}"

INSTALL_APT_DEPS="${INSTALL_APT_DEPS:-1}"
BUILD_JOBS="${BUILD_JOBS:-$(nproc)}"

ROOT_PROJECT="$(expand_tilde "$ROOT_PROJECT")"
BACKEND_DIR="$(expand_tilde "$BACKEND_DIR")"
BUILD_ROOT="$(expand_tilde "$BUILD_ROOT")"
BUILD_DIR="$(expand_tilde "$BUILD_DIR")"
INSTALL_BIN_DIR="$(expand_tilde "$INSTALL_BIN_DIR")"

install_xr_spatial_runtime_bundle() {
  [[ "$INSTALL_RUNTIME_BUNDLE" == "1" ]] || return 0

  mkdir -p "$INSTALL_BIN_DIR"

  if [[ "$INSTALL_BUNDLE_SCRIPTS" == "1" ]]; then
    mkdir -p "$INSTALL_BIN_DIR/scripts"
    rsync -a --delete "$BACKEND_DIR/scripts/" "$INSTALL_BIN_DIR/scripts/"
  fi

  if [[ "$INSTALL_BUNDLE_CONFIGS" == "1" ]]; then
    mkdir -p "$INSTALL_BIN_DIR/configs"
    rsync -a --delete "$BACKEND_DIR/configs/" "$INSTALL_BIN_DIR/configs/"
  fi

  if [[ "$INSTALL_BUNDLE_CALIBRATION" == "1" && -d "$ROOT_PROJECT/calibration_dataset" ]]; then
    mkdir -p "$INSTALL_BIN_DIR/calibration_dataset"
    rsync -a --delete "$ROOT_PROJECT/calibration_dataset/" "$INSTALL_BIN_DIR/calibration_dataset/"
  fi

  cat > "$INSTALL_BIN_DIR/README_RUN.md" <<EOF
# xr_spatial_backend portable runtime bundle

This directory is intentionally runnable without the source tree when copied to a
compatible Linux machine with the same capture/runtime stack.

Main commands:

  scripts/linux/start_xr_spatial_scan.sh
  scripts/linux/start_xr_spatial.sh
  scripts/linux/start_xr_spatial_shm.sh

The launcher detects this bin layout and uses this directory as ROOT_PROJECT,
so bundled configs can reference paths like:

  \$ROOT_PROJECT/calibration_dataset/...

If direct binary execution fails, prefer launching through scripts so the same
profile and registry defaults are applied consistently.
EOF

  if [[ "$CHECK_INSTALLED_LDD" == "1" ]]; then
    log "Checking installed xr_spatial dynamic links"
    ldd "$INSTALL_BIN_DIR/xr_spatial_backend" | tee "$INSTALL_BIN_DIR/ldd_xr_spatial_backend.txt" >&2
    if grep -q "not found" "$INSTALL_BIN_DIR/ldd_xr_spatial_backend.txt"; then
      echo "[ERROR] Installed xr_spatial_backend has missing shared libraries. See:" >&2
      echo "[ERROR]   $INSTALL_BIN_DIR/ldd_xr_spatial_backend.txt" >&2
      exit 1
    fi
  fi
}

install_legacy_spatial_mapper_compat() {
  [[ "${INSTALL_LEGACY_SPATIAL_MAPPER_COMPAT:-0}" == "1" ]] || return 0
  local legacy_dir="${LEGACY_INSTALL_BIN_DIR:-$ROOT_PROJECT/bin/backends/spatial_mapper}"
  legacy_dir="$(expand_tilde "$legacy_dir")"
  mkdir -p "$legacy_dir/scripts/linux"
  ln -sfn "../xr_spatial/xr_spatial_backend" "$legacy_dir/xr_spatial_mapper_backend" 2>/dev/null || \
    cp "$INSTALL_BIN_DIR/xr_spatial_backend" "$legacy_dir/xr_spatial_mapper_backend"
  cat > "$legacy_dir/scripts/linux/install_spatial_mapper.sh" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_PROJECT="${ROOT_PROJECT:-${XR:-$HOME/src/xr_tracking}}"
echo "[install_spatial_mapper][DEPRECATED] use backends/xr_spatial/scripts/linux/install_xr_spatial.sh" >&2
exec "$ROOT_PROJECT/backends/xr_spatial/scripts/linux/install_xr_spatial.sh" "$@"
EOF
  cat > "$legacy_dir/scripts/linux/start_spatial_mapper_shm.sh" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
ROOT_PROJECT="${ROOT_PROJECT:-${XR:-$HOME/src/xr_tracking}}"
echo "[start_spatial_mapper_shm][DEPRECATED] use backends/xr_spatial/scripts/linux/start_xr_spatial_shm.sh" >&2
exec "$ROOT_PROJECT/backends/xr_spatial/scripts/linux/start_xr_spatial_shm.sh" "$@"
EOF
  cat > "$legacy_dir/scripts/linux/start_spatial_mapper.sh" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
ROOT_PROJECT="${ROOT_PROJECT:-${XR:-$HOME/src/xr_tracking}}"
echo "[start_spatial_mapper][DEPRECATED] use backends/xr_spatial/scripts/linux/start_xr_spatial.sh" >&2
exec "$ROOT_PROJECT/backends/xr_spatial/scripts/linux/start_xr_spatial.sh" "$@"
EOF
  cat > "$legacy_dir/scripts/linux/start_spatial_scan.sh" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
ROOT_PROJECT="${ROOT_PROJECT:-${XR:-$HOME/src/xr_tracking}}"
echo "[start_spatial_scan][DEPRECATED] use backends/xr_spatial/scripts/linux/start_xr_spatial_scan.sh" >&2
exec "$ROOT_PROJECT/backends/xr_spatial/scripts/linux/start_xr_spatial_scan.sh" "$@"
EOF
  chmod +x "$legacy_dir/scripts/linux/"*.sh "$legacy_dir/xr_spatial_mapper_backend" 2>/dev/null || true
  log "Legacy spatial_mapper compatibility bundle: $legacy_dir"
}

if [[ "$INSTALL_APT_DEPS" == "1" ]]; then
  log "Installing build dependencies"
  sudo apt update
  sudo apt install -y cmake build-essential pkg-config git rsync libopencv-dev libcli11-dev nlohmann-json3-dev libeigen3-dev
fi

if [[ ! -f "$BACKEND_DIR/CMakeLists.txt" ]]; then
  echo "[ERROR] CMakeLists.txt not found: $BACKEND_DIR" >&2
  exit 1
fi

log "ROOT_PROJECT=$ROOT_PROJECT"
log "BACKEND_DIR=$BACKEND_DIR"
log "BUILD_DIR=$BUILD_DIR"
log "INSTALL_BIN_DIR=$INSTALL_BIN_DIR"
mkdir -p "$BUILD_DIR" "$INSTALL_BIN_DIR"

CMAKE_ARGS=(
  -S "$BACKEND_DIR"
  -B "$BUILD_DIR"
  -DCMAKE_BUILD_TYPE="$CMAKE_BUILD_TYPE"
  -DXR_TRACKING_ROOT="$ROOT_PROJECT"
  -DXR_SHARED_INCLUDE_DIR="$ROOT_PROJECT/shared/include"
)

cmake "${CMAKE_ARGS[@]}"
cmake --build "$BUILD_DIR" -j"$BUILD_JOBS" --target xr_spatial_backend

cp "$BUILD_DIR/xr_spatial_backend" "$INSTALL_BIN_DIR/xr_spatial_backend"
chmod +x "$INSTALL_BIN_DIR/xr_spatial_backend"

install_xr_spatial_runtime_bundle
install_legacy_spatial_mapper_compat

log "Installed: $INSTALL_BIN_DIR/xr_spatial_backend"
if [[ "$INSTALL_RUNTIME_BUNDLE" == "1" ]]; then
  log "Runtime bundle: $INSTALL_BIN_DIR"
  find "$INSTALL_BIN_DIR" -maxdepth 2 -type f \( -name "*.env" -o -name "*.sh" -o -name "*.json" -o -name "README_RUN.md" \) -printf "[install_xr_spatial] bundle %p\n" >&2 || true
fi
ls -lh "$INSTALL_BIN_DIR/xr_spatial_backend" >&2
