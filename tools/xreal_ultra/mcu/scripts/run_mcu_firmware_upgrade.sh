#!/usr/bin/env bash
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_PROJECT="${ROOT_PROJECT:-$(cd "$SCRIPT_DIR/../../../.." && pwd)}"
BIN="${MCU_UPGRADE_BIN:-${XR_BIN_ROOT:-$ROOT_PROJECT/bin}/tools/xreal_ultra/mcu/xrealAirUpgradeMCU}"
if [[ "${I_UNDERSTAND_MCU_RISK:-0}" != "1" ]]; then
  cat >&2 <<'MSG'
[run_mcu_firmware_upgrade][BLOCKED]
Firmware upgrade can permanently damage or brick the glasses.
This script is disabled by default.

To run it anyway, set:
  I_UNDERSTAND_MCU_RISK=1
MSG
  exit 2
fi
if [[ ! -x "$BIN" ]]; then
  echo "[run_mcu_firmware_upgrade][ERROR] binary not found: $BIN" >&2
  echo "Build first: tools/xreal_ultra/mcu/scripts/build_mcu_tools.sh" >&2
  exit 1
fi
exec "$BIN" "$@"
