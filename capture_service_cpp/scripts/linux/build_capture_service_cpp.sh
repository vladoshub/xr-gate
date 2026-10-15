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
BUILD_SUPPORT_DIR="$ROOT_PROJECT/devices/common/linux/scripts/build"
if [[ -z "${XR_BUILD_SESSION_DIR:-}" || ! -d "$XR_BUILD_SESSION_DIR" ]] || [[ -z "${XR_BUILD_SESSION_OWNER:-}" ]] || ! kill -0 "${XR_BUILD_SESSION_OWNER:-0}" 2>/dev/null; then
  exec bash "$BUILD_SUPPORT_DIR/with_build_session.sh" bash "$0" "$@"
fi
if [[ "${XR_USE_BUNDLED_OPENCV:-1}" == "1" ]]; then
  source "$BUILD_SUPPORT_DIR/opencv_env.sh"
fi

INSTALL_BIN_DIR="${INSTALL_BIN_DIR:-${XR_BIN_ROOT:-$ROOT_PROJECT/bin}/capture_service_cpp}"
INSTALL_BIN_DIR="$(expand_tilde "$INSTALL_BIN_DIR")"
BUILD_DIR="${BUILD_DIR:-$XR_BUILD_ROOT/capture_service_cpp}"
BUILD_DIR="$(expand_tilde "$BUILD_DIR")"

mkdir -p "$BUILD_DIR" "$INSTALL_BIN_DIR"
echo "[build_capture_service_cpp] runtime-selectable camera/IMU source build" >&2
# CMake's imported OpenCV targets carry the transitive static codec libraries.
# Compile separate objects instead of passing every source to one g++ invocation.
cmake -S "$BACKEND_DIR" -B "$BUILD_DIR" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF \
  ${XR_OPENCV_PREFIX:+"-DOpenCV_DIR=$XR_OPENCV_PREFIX/lib/cmake/opencv4"} \
  -DCMAKE_CXX_FLAGS="${CXXFLAGS:-} ${CXXFLAGS_EXTRA:-}" \
  -DCMAKE_EXE_LINKER_FLAGS="${LDFLAGS_EXTRA:-}"
cmake --build "$BUILD_DIR" --target capture_service_cpp capture_tcp_probe --parallel "${BUILD_JOBS:-2}"
cp "$BUILD_DIR/capture_service_cpp" "$BUILD_DIR/capture_tcp_probe" "$INSTALL_BIN_DIR/"
if [[ -n "${XR_OPENCV_PREFIX:-}" ]]; then
  mkdir -p "$INSTALL_BIN_DIR/licenses"
  cp -a "$XR_OPENCV_PREFIX/licenses" "$INSTALL_BIN_DIR/licenses/opencv"
fi

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
