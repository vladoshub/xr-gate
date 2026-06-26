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
ROOT_PROJECT="${ROOT_PROJECT:-$(cd "$SCRIPT_DIR/../../../.." && pwd)}"
ROOT_PROJECT="$(expand_tilde "$ROOT_PROJECT")"
NREAL_DRIVER_DIR="${NREAL_DRIVER_DIR:-$ROOT_PROJECT/third_party/nrealAirLinuxDriver}"
NREAL_DRIVER_DIR="$(expand_tilde "$NREAL_DRIVER_DIR")"
BUILD_DIR="${BUILD_DIR:-$ROOT_PROJECT/build/tools/xreal_ultra/mcu}"
BUILD_DIR="$(expand_tilde "$BUILD_DIR")"
INSTALL_BIN_DIR="${INSTALL_BIN_DIR:-${XR_BIN_ROOT:-$ROOT_PROJECT/bin}/tools/xreal_ultra/mcu}"
INSTALL_BIN_DIR="$(expand_tilde "$INSTALL_BIN_DIR")"
CC_BIN="${CC:-cc}"

if [[ ! -d "$NREAL_DRIVER_DIR" ]]; then
  echo "[build_mcu_tools][ERROR] vendored upstream snapshot not found: $NREAL_DRIVER_DIR" >&2
  echo "Expected source-controlled third-party snapshot under third_party/nrealAirLinuxDriver." >&2
  echo "Refresh/install from a reviewed archive if needed:" >&2
  echo "  tools/xreal_ultra/mcu/scripts/prepare_nreal_upstream_from_archive.sh /path/to/nrealAirLinuxDriver.zip" >&2
  exit 2
fi

REQUIRED_FILES=(
  "$NREAL_DRIVER_DIR/LICENSE.md"
  "$NREAL_DRIVER_DIR/interface_lib/include/device_mcu.h"
  "$NREAL_DRIVER_DIR/interface_lib/include/device.h"
  "$NREAL_DRIVER_DIR/interface_lib/include/crc32.h"
  "$NREAL_DRIVER_DIR/interface_lib/include/hid_ids.h"
  "$NREAL_DRIVER_DIR/interface_lib/include/endian_compat.h"
  "$NREAL_DRIVER_DIR/interface_lib/src/device.c"
  "$NREAL_DRIVER_DIR/interface_lib/src/device_mcu.c"
  "$NREAL_DRIVER_DIR/interface_lib/src/crc32.c"
  "$NREAL_DRIVER_DIR/interface_lib/src/hid_ids.c"
  "$NREAL_DRIVER_DIR/examples/debug_mcu/src/debug.c"
  "$NREAL_DRIVER_DIR/examples/mcu_firmware/src/upgrade.c"
)
for path in "${REQUIRED_FILES[@]}"; do
  if [[ ! -f "$path" ]]; then
    echo "[build_mcu_tools][ERROR] required upstream file missing: $path" >&2
    echo "Use a full nrealAirLinuxDriver checkout/archive, not examples-only." >&2
    exit 2
  fi
done

HID_CFLAGS=""
HID_LIBS=""
if pkg-config --exists hidapi-hidraw; then
  HID_CFLAGS="$(pkg-config --cflags hidapi-hidraw)"
  HID_LIBS="$(pkg-config --libs hidapi-hidraw)"
elif pkg-config --exists hidapi-libusb; then
  HID_CFLAGS="$(pkg-config --cflags hidapi-libusb)"
  HID_LIBS="$(pkg-config --libs hidapi-libusb)"
else
  echo "[build_mcu_tools][WARN] pkg-config entry for hidapi was not found; trying default hidapi-hidraw link flags." >&2
  HID_CFLAGS="-I/usr/include/hidapi"
  HID_LIBS="-lhidapi-hidraw"
fi

mkdir -p "$BUILD_DIR" "$INSTALL_BIN_DIR"

COMMON_SOURCES=(
  "$NREAL_DRIVER_DIR/interface_lib/src/device.c"
  "$NREAL_DRIVER_DIR/interface_lib/src/device_mcu.c"
  "$NREAL_DRIVER_DIR/interface_lib/src/crc32.c"
  "$NREAL_DRIVER_DIR/interface_lib/src/hid_ids.c"
)
COMMON_FLAGS=(
  -std=c17
  -D_DEFAULT_SOURCE=1
  -O2
  -Wall
  -Wextra
  -Wno-unused-parameter
  -I"$NREAL_DRIVER_DIR/interface_lib/include"
)

# Deliberately build only the MCU examples directly against system hidapi.
# Upstream CMake also builds IMU/driver targets and expects git submodules for
# hidapi/Fusion; those are unnecessary for MCU debug/firmware tools and make the
# optional toolchain harder to use from a source/package checkout.
echo "[build_mcu_tools] building xrealAirDebugMCU"
# shellcheck disable=SC2086
"$CC_BIN" "${COMMON_FLAGS[@]}" $HID_CFLAGS \
  "$NREAL_DRIVER_DIR/examples/debug_mcu/src/debug.c" \
  "${COMMON_SOURCES[@]}" \
  -o "$BUILD_DIR/xrealAirDebugMCU" $HID_LIBS

echo "[build_mcu_tools] building xrealAirUpgradeMCU"
# shellcheck disable=SC2086
"$CC_BIN" "${COMMON_FLAGS[@]}" $HID_CFLAGS \
  "$NREAL_DRIVER_DIR/examples/mcu_firmware/src/upgrade.c" \
  "${COMMON_SOURCES[@]}" \
  -o "$BUILD_DIR/xrealAirUpgradeMCU" $HID_LIBS

install -m 0755 "$BUILD_DIR/xrealAirDebugMCU" "$INSTALL_BIN_DIR/xrealAirDebugMCU"
install -m 0755 "$BUILD_DIR/xrealAirUpgradeMCU" "$INSTALL_BIN_DIR/xrealAirUpgradeMCU"
cp -a "$ROOT_PROJECT/tools/xreal_ultra/mcu/README.md" "$INSTALL_BIN_DIR/README.md" 2>/dev/null || true
cp -a "$NREAL_DRIVER_DIR/LICENSE.md" "$INSTALL_BIN_DIR/LICENSE.nrealAirLinuxDriver.md"

cat <<EOF2
[build_mcu_tools] installed MCU tools into: $INSTALL_BIN_DIR
[build_mcu_tools] source: $NREAL_DRIVER_DIR
[build_mcu_tools] license: $NREAL_DRIVER_DIR/LICENSE.md
[build_mcu_tools] note: tools are unsafe/opt-in and are not used by default runtime
EOF2
