#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../../.." && pwd)"
MONADO_DIR="${MONADO_DIR:-$ROOT/third_party/monado_driver}"
TEST_BUILD_DIR="$(mktemp -d)"
trap 'rm -rf "$TEST_BUILD_DIR"' EXIT
args=(-std=c++17 -O1 -g -Wall -Wextra -pthread -DXR_GATE_USE_SHARED_HAND_CONTRACT=1
  -I"$ROOT/drivers/monado_driver/include" -I"$ROOT/drivers/openvr_driver/src" -I"$ROOT/shared/include"
  -I"$MONADO_DIR/src/external/openxr_includes")
if [[ -n "${NLOHMANN_JSON_INCLUDE_DIR:-}" ]]; then args+=(-I"$NLOHMANN_JSON_INCLUDE_DIR"); fi
if [[ "${SANITIZE:-0}" == 1 ]]; then args+=(-fsanitize=address,undefined -fno-omit-frame-pointer); fi
"${CXX:-c++}" "${args[@]}" "$ROOT/drivers/monado_driver/tests/openxr_data_test.cpp" \
  "$ROOT/drivers/monado_driver/openxr/sensor_source.cpp" \
  "$ROOT/drivers/monado_driver/src/runtime_readers.cpp" \
  "$ROOT/drivers/monado_driver/src/posix_runtime_readers.cpp" \
  "$ROOT/drivers/openvr_driver/src/runtime_tcp_client.cpp" \
  "$ROOT/drivers/openvr_driver/src/runtime_tcp_readers.cpp" \
  "$ROOT/runtime_adapters/xr_runtime_adapter/src/app_tcp_server.cpp" \
  -lrt -o "$TEST_BUILD_DIR/openxr_data_test"
"$TEST_BUILD_DIR/openxr_data_test"
