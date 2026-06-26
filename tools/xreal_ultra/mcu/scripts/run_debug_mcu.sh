#!/usr/bin/env bash
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_PROJECT="${ROOT_PROJECT:-$(cd "$SCRIPT_DIR/../../../.." && pwd)}"
BIN="${MCU_DEBUG_BIN:-${XR_BIN_ROOT:-$ROOT_PROJECT/bin}/tools/xreal_ultra/mcu/xrealAirDebugMCU}"
if [[ ! -x "$BIN" ]]; then
  echo "[run_debug_mcu][ERROR] binary not found: $BIN" >&2
  echo "Build first: tools/xreal_ultra/mcu/scripts/build_mcu_tools.sh" >&2
  exit 1
fi
cat >&2 <<'MSG'
[run_debug_mcu][WARNING] This tool talks directly to the XREAL/NREAL MCU.
It is not part of the normal runtime and is provided without safety guarantees.
MSG
exec "$BIN" "$@"
