#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=load_target.sh
source "$SCRIPT_DIR/load_target.sh"

BAG="${BAG:-$(ls -1t "$BAGS_DIR"/$BAG_PATTERN 2>/dev/null | head -n1 || true)}"
[[ -n "$BAG" ]] || { echo "[save-camera][ERROR] no bag matching $BAGS_DIR/$BAG_PATTERN" >&2; exit 1; }
BAG="$(expand_tilde "$BAG")"
OUT="$(expand_tilde "${OUT:-$KALIBR_RUNS_DIR/camera_$(basename "$BAG" .bag)}")"
mkdir -p "$CAMERA_PROFILE_DIR"

CAMCHAIN="$(find "$OUT" -maxdepth 1 -name 'camchain-*.yaml' | head -n1)"
RESULTS="$(find "$OUT" -maxdepth 1 -name 'results-cam-*.txt' | head -n1)"
[[ -n "$CAMCHAIN" ]] || { echo "[save-camera][ERROR] camchain not found in $OUT" >&2; exit 1; }

CAMCHAIN_FINAL="$CAMERA_PROFILE_DIR/$CAMCHAIN_OUTPUT_NAME"
RESULTS_FINAL="$CAMERA_PROFILE_DIR/$RESULTS_CAMERA_OUTPUT_NAME"
cp "$CAMCHAIN" "$CAMCHAIN_FINAL"
[[ -n "$RESULTS" ]] && cp "$RESULTS" "$RESULTS_FINAL"
cp "$APRILGRID_PATH" "$CAMERA_PROFILE_DIR/aprilgrid.yaml"

echo "[OK] installed camera profile:"
ls -lh "$CAMERA_PROFILE_DIR"
echo
echo "[resolution check]"
grep -nA8 'resolution' "$CAMCHAIN_FINAL" || true
