#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../../.." && pwd)"
TEST_BUILD_DIR="$(mktemp -d)"
trap 'rm -rf "$TEST_BUILD_DIR"' EXIT
args=(-DXR_GATE_USE_SHARED_HAND_CONTRACT=1 -std=c++17 -O1 -g -Wall -Wextra -pthread
  -I"$ROOT/drivers/monado_driver/include" -I"$ROOT/drivers/openvr_driver/src" -I"$ROOT/shared/include")
if [[ -n "${NLOHMANN_JSON_INCLUDE_DIR:-}" ]]; then
  args+=(-I"$NLOHMANN_JSON_INCLUDE_DIR")
fi
if [[ "${SANITIZE:-0}" == 1 ]]; then
  args+=(-fsanitize=address,undefined -fno-omit-frame-pointer)
fi
"${CXX:-c++}" "${args[@]}" \
  "$ROOT/drivers/monado_driver/tests/posix_shm_test.cpp" \
  "$ROOT/drivers/monado_driver/src/runtime_readers.cpp" \
  "$ROOT/drivers/monado_driver/src/posix_runtime_readers.cpp" \
  "$ROOT/drivers/openvr_driver/src/runtime_tcp_client.cpp" \
  "$ROOT/drivers/openvr_driver/src/runtime_tcp_readers.cpp" \
  -lrt -o "$TEST_BUILD_DIR/posix_shm_test"
"$TEST_BUILD_DIR/posix_shm_test"
