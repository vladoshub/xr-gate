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

ROOT_PROJECT="${ROOT_PROJECT:-$HOME/src/xr_tracking}"
ROOT_PROJECT="$(expand_tilde "$ROOT_PROJECT")"

XR_PACKAGE_ROOT="${XR_PACKAGE_ROOT:-$ROOT_PROJECT/out/xreal_ultra}"
XR_PACKAGE_ROOT="$(expand_tilde "$XR_PACKAGE_ROOT")"

if [[ -n "${CAPTURE_CLIENT_ROOT:-}" ]]; then
  CAPTURE_CLIENT_ROOT="$(expand_tilde "$CAPTURE_CLIENT_ROOT")"
elif [[ -d "$ROOT_PROJECT/capture_client" ]]; then
  CAPTURE_CLIENT_ROOT="$ROOT_PROJECT/capture_client"
elif [[ -d "$XR_PACKAGE_ROOT/bin/python/capture_client" ]]; then
  CAPTURE_CLIENT_ROOT="$XR_PACKAGE_ROOT/bin/python/capture_client"
else
  CAPTURE_CLIENT_ROOT="$ROOT_PROJECT/capture_client"
fi

CAPTURE_CLIENT_PYTHONPATH="${CAPTURE_CLIENT_PYTHONPATH:-$(dirname "$CAPTURE_CLIENT_ROOT")}"
VIEWER_SCRIPT="${VIEWER_SCRIPT:-$CAPTURE_CLIENT_ROOT/debug/xreal_slam_viewer.py}"
VIEWER_SCRIPT="$(expand_tilde "$VIEWER_SCRIPT")"

if [[ -n "${XR_PYTHON_BIN:-}" ]]; then
  PYTHON_BIN="$(expand_tilde "$XR_PYTHON_BIN")"
elif [[ -x "$XR_PACKAGE_ROOT/bin/python-runtime/venv/bin/python" ]]; then
  PYTHON_BIN="$XR_PACKAGE_ROOT/bin/python-runtime/venv/bin/python"
elif [[ -x "$ROOT_PROJECT/out/xreal_ultra/bin/python-runtime/venv/bin/python" ]]; then
  PYTHON_BIN="$ROOT_PROJECT/out/xreal_ultra/bin/python-runtime/venv/bin/python"
else
  PYTHON_BIN="${PYTHON_BIN:-python3}"
fi

REGISTRY="${REGISTRY:-/tmp/capture_service_streams.json}"
CAM0_STREAM="${CAM0_STREAM:-camera0}"
CAM1_STREAM="${CAM1_STREAM:-camera1}"
MAX_DELTA_MS="${MAX_DELTA_MS:-1.0}"
SCALE="${SCALE:-1.0}"
WINDOW="${WINDOW:-XREAL SLAM stereo viewer SHM}"

if [[ ! -d "$CAPTURE_CLIENT_ROOT" ]]; then
  echo "[ERROR] CAPTURE_CLIENT_ROOT does not exist: $CAPTURE_CLIENT_ROOT" >&2
  echo "Set CAPTURE_CLIENT_ROOT or run from a checkout/package containing capture_client." >&2
  exit 1
fi
if [[ ! -f "$VIEWER_SCRIPT" ]]; then
  echo "[ERROR] viewer script not found: $VIEWER_SCRIPT" >&2
  exit 1
fi
if ! command -v "$PYTHON_BIN" >/dev/null 2>&1 && [[ ! -x "$PYTHON_BIN" ]]; then
  echo "[ERROR] Python not found: $PYTHON_BIN" >&2
  echo "Set XR_PYTHON_BIN or install the package runtime dependencies." >&2
  exit 1
fi

export CAPTURE_CLIENT_ROOT
export PYTHONPATH="$CAPTURE_CLIENT_PYTHONPATH:${PYTHONPATH:-}"

cd "$CAPTURE_CLIENT_PYTHONPATH"

echo "CAPTURE_CLIENT_ROOT=$CAPTURE_CLIENT_ROOT"
echo "VIEWER_SCRIPT=$VIEWER_SCRIPT"
echo "PYTHON_BIN=$PYTHON_BIN"
echo "REGISTRY=$REGISTRY"
echo "CAM0_STREAM=$CAM0_STREAM"
echo "CAM1_STREAM=$CAM1_STREAM"

exec "$PYTHON_BIN" "$VIEWER_SCRIPT" \
  --transport shm \
  --registry "$REGISTRY" \
  --cam0-stream "$CAM0_STREAM" \
  --cam1-stream "$CAM1_STREAM" \
  --max-delta-ms "$MAX_DELTA_MS" \
  --scale "$SCALE" \
  --window "$WINDOW"
