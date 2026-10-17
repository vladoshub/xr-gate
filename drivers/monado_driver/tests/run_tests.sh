#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../../.." && pwd)"
MONADO_DIR="${MONADO_DIR:-$ROOT/third_party/monado_driver}"
if [[ ! -f "$MONADO_DIR/src/xrt/include/xrt/xrt_device.h" ]]; then
  echo "Set MONADO_DIR to the Monado checkout used by drivers/monado_driver/scripts/linux/install.sh" >&2
  exit 1
fi
TEST_BUILD_DIR="$(mktemp -d)"
trap 'rm -rf "$TEST_BUILD_DIR"' EXIT
args=(-std=c++17 -O1 -g -Wall -Wextra -pthread
  -I"$ROOT/drivers/monado_driver/include" -I"$ROOT/shared/include"
  -I"$MONADO_DIR/src/xrt/include" -I"$MONADO_DIR/src/xrt/auxiliary")
if [[ -n "${NLOHMANN_JSON_INCLUDE_DIR:-}" ]]; then
  args+=(-I"$NLOHMANN_JSON_INCLUDE_DIR")
fi
if [[ "${SANITIZE:-0}" == 1 ]]; then
  args+=(-fsanitize=address,undefined -fno-omit-frame-pointer)
fi
"${CXX:-c++}" "${args[@]}" \
  "$ROOT/drivers/monado_driver/tests/monado_driver_test.cpp" \
  "$ROOT/drivers/monado_driver/src/runtime_readers.cpp" \
  "$ROOT/drivers/monado_driver/src/posix_runtime_readers.cpp" \
  -lrt -o "$TEST_BUILD_DIR/monado_driver_test"
"$TEST_BUILD_DIR/monado_driver_test"
