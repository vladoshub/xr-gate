#!/usr/bin/env bash
set -euo pipefail
app_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
build_dir="${XR_HAND_BUILD_DIR:-$app_dir/build}"
if [[ "${1:-}" == --help ]]; then
  cat <<'HELP'
Usage: bash run.sh [--runtime /absolute/openxr_monado_xrgate.json] [app options]
Builds the example and starts it. Start XR Gate tracking and Monado separately.
Uses XR_RUNTIME_JSON unchanged unless --runtime is supplied.
App options: --body, --joints 21|26, --no-mirror, --demo, --frames N, --help
Use --body for FRAME body trackers. G toggles the grid and Esc exits.
HELP
  exit 0
fi
if [[ "${1:-}" == --runtime ]]; then
  [[ $# -ge 2 && -f "$2" ]] || { echo 'Missing runtime manifest after --runtime' >&2; exit 2; }
  export XR_RUNTIME_JSON="$(realpath -- "$2")"
  shift 2
fi
command -v cmake >/dev/null || { echo 'Install build dependencies listed in README.md (cmake is missing).' >&2; exit 1; }
cmake -S "$app_dir" -B "$build_dir" -DCMAKE_BUILD_TYPE=Release
cmake --build "$build_dir" --parallel "${XR_HAND_BUILD_JOBS:-2}"
echo "[skeleton] XR_RUNTIME_JSON=${XR_RUNTIME_JSON:-<OpenXR loader default>}"
exec "$build_dir/xr_gate_hand_skeleton" "$@"
