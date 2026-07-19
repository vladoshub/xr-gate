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

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BACKEND_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
ROOT_PROJECT="${ROOT_PROJECT:-$(cd "$BACKEND_DIR/.." && pwd)}"
ROOT_PROJECT="$(expand_tilde "$ROOT_PROJECT")"

INSTALL_BIN_DIR="${INSTALL_BIN_DIR:-${XR_BIN_ROOT:-$ROOT_PROJECT/bin}/capture_service_cpp}"
INSTALL_BIN_DIR="$(expand_tilde "$INSTALL_BIN_DIR")"
BUILD_DIR="${BUILD_DIR:-$ROOT_PROJECT/build/capture_service_cpp}"
BUILD_DIR="$(expand_tilde "$BUILD_DIR")"

CXX="${CXX:-g++}"
mkdir -p "$BUILD_DIR" "$INSTALL_BIN_DIR"
echo "[build_capture_service_cpp] runtime-selectable camera/IMU source build" >&2

if ! pkg-config --exists opencv4; then
  echo "[build_capture_service_cpp][ERROR] pkg-config package opencv4 not found" >&2
  echo "Install: sudo apt install -y libopencv-dev pkg-config" >&2
  exit 2
fi

HIDAPI_PKG="${HIDAPI_PKG:-}"
if [[ -z "$HIDAPI_PKG" ]]; then
  if pkg-config --exists hidapi-hidraw; then
    HIDAPI_PKG="hidapi-hidraw"
  elif pkg-config --exists hidapi-libusb; then
    HIDAPI_PKG="hidapi-libusb"
  else
    echo "[build_capture_service_cpp][ERROR] hidapi pkg-config package not found" >&2
    echo "Install: sudo apt install -y libhidapi-dev" >&2
    exit 2
  fi
fi

CXXFLAGS_EXTRA="${CXXFLAGS_EXTRA:-}"
LDFLAGS_EXTRA="${LDFLAGS_EXTRA:-}"

# Include all platform-neutral sources recursively, then only Linux platform
# implementations. Windows implementations are excluded explicitly.
mapfile -t SRC_FILES < <(
  {
    find "$BACKEND_DIR/src" -type f -name '*.cpp' \
      ! -path '*/platform/linux/*' \
      ! -path '*/platform/windows/*'
    find "$BACKEND_DIR/src/platform/linux" -type f -name '*.cpp'
  } | sort
)

set -x
"$CXX" -std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter \
  -I"$BACKEND_DIR/include" \
  "${SRC_FILES[@]}" \
  -o "$INSTALL_BIN_DIR/capture_service_cpp" \
  $(pkg-config --cflags opencv4 "$HIDAPI_PKG") \
  $CXXFLAGS_EXTRA \
  $(pkg-config --libs opencv4 "$HIDAPI_PKG") \
  -pthread $LDFLAGS_EXTRA
set +x

set -x
"$CXX" -std=c++17 -O2 -Wall -Wextra -Werror -pedantic \
  "$BACKEND_DIR/tools/capture_tcp_probe.cpp" \
  -o "$INSTALL_BIN_DIR/capture_tcp_probe" \
  $CXXFLAGS_EXTRA $LDFLAGS_EXTRA
set +x

if [[ -d "$BACKEND_DIR/configs" ]]; then
  rm -rf "$INSTALL_BIN_DIR/configs"
  cp -a "$BACKEND_DIR/configs" "$INSTALL_BIN_DIR/configs"
fi
mkdir -p "$INSTALL_BIN_DIR/tools"
cp "$BACKEND_DIR/tools/list_xr_controller_devices.py" \
  "$INSTALL_BIN_DIR/tools/list_xr_controller_devices.py"
chmod +x "$INSTALL_BIN_DIR/tools/list_xr_controller_devices.py"

cat > "$INSTALL_BIN_DIR/README.md" <<'README'
# capture_service_cpp runtime

The camera and IMU sources are selected independently at runtime through:

```text
~/.config/xr_tracking/capture_service_cpp/config.yaml
```

An exact path can be selected with `--config`, or a directory/name pair with
`--config-dir` and `--config-name`.
README

echo "[build_capture_service_cpp] installed: $INSTALL_BIN_DIR/capture_service_cpp"
echo "[build_capture_service_cpp] installed: $INSTALL_BIN_DIR/capture_tcp_probe"
echo "[build_capture_service_cpp] installed: $INSTALL_BIN_DIR/tools/list_xr_controller_devices.py"
