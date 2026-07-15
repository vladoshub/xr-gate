#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

export STARTUP_GATE="${STARTUP_GATE:-1}"
# Preserve the historical debug gate: visual checks only, with the stricter
# corner/grid thresholds used by the old standalone debug launcher.
export STARTUP_GATE_VISUAL="${STARTUP_GATE_VISUAL:-1}"
export STARTUP_GATE_IMU="${STARTUP_GATE_IMU:-0}"
export STARTUP_MIN_CORNERS="${STARTUP_MIN_CORNERS:-260}"
export STARTUP_MIN_GRID_CELLS="${STARTUP_MIN_GRID_CELLS:-14}"
export OUT_DIR="${OUT_DIR:-/tmp/xr_basalt_unified_live}"
rm -rf "$OUT_DIR"
mkdir -p "$OUT_DIR"

exec "$SCRIPT_DIR/../start_basalt.sh" \
  --save-trajectory \
  --trajectory "$OUT_DIR/trajectory.csv" \
  --print-every "${PRINT_EVERY:-30}" \
  "$@"
