#!/usr/bin/env bash
set -euo pipefail

# Reuse a configured Mercury build; never reset/fetch the source tree.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_PROJECT="${ROOT_PROJECT:-$(cd "$SCRIPT_DIR/../../../.." && pwd)}"
BACKEND_DIR="$ROOT_PROJECT/backends/mercury_hand_tracking"
DIAG_PATCH="$BACKEND_DIR/patches/monado/mercury_tracking_diagnostics.patch"
DIAG_HEADER="$BACKEND_DIR/monado_overlay/src/xrt/tracking/hand/mercury/xr_mercury_tracking_diagnostics.hpp"
QUALITY_PATCH="$BACKEND_DIR/patches/monado/mercury_quality_filter.patch"
QUALITY_HEADER="$BACKEND_DIR/monado_overlay/src/xrt/tracking/hand/mercury/xr_mercury_quality_filter.hpp"

fail() { echo "[mercury_tracking_diag][ERROR] $*" >&2; exit 1; }
command -v cmake >/dev/null || fail "cmake is required"
[[ -f "$DIAG_PATCH" && -f "$DIAG_HEADER" ]] || fail "diagnostic source files are missing"
[[ -f "$QUALITY_PATCH" && -f "$QUALITY_HEADER" ]] || fail "quality filter source files are missing"

if [[ -z "${MERCURY_BUILD_DIR:-}" ]]; then
  for candidate in \
    "$ROOT_PROJECT/build/backends/mercury_hand_tracking/monado_xr_mercury" \
    "$ROOT_PROJECT/third_party/monado/build/xr_tracking_relwithdebinfo" \
    "$ROOT_PROJECT/third_party/monado_driver/build/xr_tracking_relwithdebinfo"
  do
    if [[ -f "$candidate/CMakeCache.txt" ]]; then
      MERCURY_BUILD_DIR="$candidate"
      break
    fi
  done
fi
[[ -n "${MERCURY_BUILD_DIR:-}" && -f "$MERCURY_BUILD_DIR/CMakeCache.txt" ]] || \
  fail "configured Mercury build not found; set MERCURY_BUILD_DIR or run scripts/linux/install_mercury.sh"

CACHE_SOURCE="$(sed -n 's/^CMAKE_HOME_DIRECTORY:INTERNAL=//p' "$MERCURY_BUILD_DIR/CMakeCache.txt")"
[[ -n "$CACHE_SOURCE" ]] || fail "CMake source directory is missing from the cache"
MONADO_DIR="${MONADO_DIR:-$CACHE_SOURCE}"
[[ "$(cd "$MONADO_DIR" && pwd -P)" == "$(cd "$CACHE_SOURCE" && pwd -P)" ]] || \
  fail "MONADO_DIR differs from the source configured in MERCURY_BUILD_DIR"
NATIVE_DIR="$MONADO_DIR/src/xrt/tracking/hand/mercury"
[[ -f "$NATIVE_DIR/hg_sync.cpp" ]] || fail "Mercury source not found in $MONADO_DIR"

# The quality patch depends on the diagnostics patch and changes adjacent lines.
# Check the newest stage first so a repeat run does not reverse-check its predecessor.
if ! git -C "$MONADO_DIR" apply --reverse --check "$QUALITY_PATCH" >/dev/null 2>&1; then
  if ! git -C "$MONADO_DIR" apply --reverse --check "$DIAG_PATCH" >/dev/null 2>&1; then
    git -C "$MONADO_DIR" apply --check "$DIAG_PATCH" || \
      fail "diagnostic patch does not match this Monado tree"
    git -C "$MONADO_DIR" apply "$DIAG_PATCH"
  fi
  git -C "$MONADO_DIR" apply --check "$QUALITY_PATCH" || \
    fail "quality patch does not match this Monado tree"
  git -C "$MONADO_DIR" apply "$QUALITY_PATCH"
fi

install -m 644 "$DIAG_HEADER" "$NATIVE_DIR/xr_mercury_tracking_diagnostics.hpp"
install -m 644 "$QUALITY_HEADER" "$NATIVE_DIR/xr_mercury_quality_filter.hpp"
echo "[mercury_tracking_diag] rebuilding library in $MERCURY_BUILD_DIR"
cmake --build "$MERCURY_BUILD_DIR" --target xr_mercury_runtime -j"${BUILD_JOBS:-$(nproc)}"
RUNTIME_SO="$MERCURY_BUILD_DIR/src/xrt/tracking/hand/mercury/libxr_mercury_runtime.so"
[[ -f "$RUNTIME_SO" ]] || fail "rebuilt library not found: $RUNTIME_SO"

INSTALL_BIN_DIR="${INSTALL_BIN_DIR:-$ROOT_PROJECT/bin/backends/mercury_hand_tracking}"
mkdir -p "$INSTALL_BIN_DIR"
DEST="$INSTALL_BIN_DIR/libxr_mercury_runtime.so"
if [[ -f "$DEST" && ! -e "$DEST.before_tracking_diagnostics" ]]; then
  cp -p "$DEST" "$DEST.before_tracking_diagnostics"
fi
TMP_DEST="$(mktemp "$INSTALL_BIN_DIR/.mercury-diag.XXXXXX")"
trap 'rm -f "$TMP_DEST"' EXIT
install -m 755 "$RUNTIME_SO" "$TMP_DEST"
mv -f "$TMP_DEST" "$DEST"
echo "[mercury_tracking_diag] installed $DEST"
echo "Restart hand tracking with MERCURY_XR_TRACKING_DEBUG_CSV set to an absolute CSV path."
echo "The quality filter is disabled by default; MERCURY_XR_QUALITY_FILTER=1 enables it for comparison."
