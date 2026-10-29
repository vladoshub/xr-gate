#!/usr/bin/env bash
# Explicit refresh step: run the selected build online, retaining pinned refs.
# Forward the same selection/profile env as for a normal build. An alternative
# build entrypoint (e.g. the device-specific wrapper) can be passed as arguments.
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
if [[ "${1:-}" == --help ]]; then
  echo "Usage: $0 [build-command [args...]]"
  echo "Refresh remote dependency metadata and rebuild (default: install_xr_gate_out.sh)."
  echo "Pinned commits and Cargo.lock are kept; system packages are prepared separately."
  exit 0
fi
[[ $# -gt 0 ]] || set -- bash "$SCRIPT_DIR/install_xr_gate_out.sh"
export XR_OFFLINE=0 XR_UPDATE_DEPS=1
exec "$@"
