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

# Device target is intentionally explicit even though this backend currently has
# only one vendor implementation. It keeps the xreal_ultra device build scripts
# from hard-coding an implicit capture target and gives future devices a clean
# place to select their own vendor/platform sources.
XR_CAPTURE_SERVICE_CPP_DEVICE="${XR_CAPTURE_SERVICE_CPP_DEVICE:-${XR_CAPTURE_DEVICE:-${XR_DEVICE_TARGET:-${XR_TARGET_DEVICE:-xreal_ultra}}}}"
case "${XR_CAPTURE_SERVICE_CPP_DEVICE}" in
  xreal_ultra|xreal_air2ultra)
    XR_CAPTURE_SERVICE_CPP_DEVICE="xreal_ultra"
    ;;
  *)
    echo "[build_capture_service_cpp][ERROR] Unsupported capture_service_cpp device: ${XR_CAPTURE_SERVICE_CPP_DEVICE}" >&2
    echo "[build_capture_service_cpp][ERROR] Supported devices: xreal_ultra" >&2
    exit 2
    ;;
esac

CXX="${CXX:-g++}"
mkdir -p "$BUILD_DIR" "$INSTALL_BIN_DIR"
echo "[build_capture_service_cpp] device=${XR_CAPTURE_SERVICE_CPP_DEVICE}" >&2

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

# Keep the direct g++ build path in sync with the CMake source list.
# The platform/vendor split moved implementations out of src/*.cpp, so a
# shallow source search links only declarations and fails with undefined
# references. This Linux build script must include:
#   - root service sources
#   - platform-neutral adapters under src/platform
#   - Linux platform implementations under src/platform/linux
#   - XREAL vendor implementations under src/vendor
# Do not include src/platform/windows here.
mapfile -t SRC_FILES < <(
  {
    find "$BACKEND_DIR/src" -maxdepth 1 -type f -name '*.cpp'
    find "$BACKEND_DIR/src/platform" -maxdepth 1 -type f -name '*.cpp'
    find "$BACKEND_DIR/src/platform/linux" -maxdepth 1 -type f -name '*.cpp'
    find "$BACKEND_DIR/src/vendor" -maxdepth 1 -type f -name '*.cpp'
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

cat > "$INSTALL_BIN_DIR/README.md" <<'README'
# capture_service_cpp runtime

Device build target: `${XR_CAPTURE_SERVICE_CPP_DEVICE:-xreal_ultra}`.

Experimental C++ capture service.

Linux SHM mode:

```bash
CAPTURE_SERVICE_IMPL=cpp XR_ENABLE_EXPERIMENTAL_CAPTURE_CPP=1 devices/xreal_ultra/linux/scripts/capture_service/start_capture_service.sh
```

TCP mode:

```bash
CAPTURE_SERVICE_IMPL=cpp XR_ENABLE_EXPERIMENTAL_CAPTURE_CPP=1 PUBLISH=tcp TCP_PORT=45660 devices/xreal_ultra/linux/scripts/capture_service/start_capture_service.sh
```
README

echo "[build_capture_service_cpp] installed: $INSTALL_BIN_DIR/capture_service_cpp"
