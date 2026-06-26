#!/usr/bin/env bash
set -euo pipefail

log() { echo "[register_xrizer_openvrpaths] $*" >&2; }
fail() { echo "[register_xrizer_openvrpaths][ERROR] $*" >&2; exit 1; }

expand_path() {
  local path="$1"
  if [[ "$path" == "~" ]]; then
    printf '%s\n' "$HOME"
  elif [[ "$path" == "~/"* ]]; then
    printf '%s/%s\n' "$HOME" "${path#~/}"
  else
    printf '%s\n' "$path"
  fi
}

find_package_or_project_root() {
  local d
  d="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
  while [[ "$d" != "/" && -n "$d" ]]; do
    if [[ -f "$d/devices/xreal_ultra/xreal_ultra.env" && -d "$d/bin" ]]; then
      printf '%s\n' "$d"
      return 0
    fi
    if [[ -d "$d/drivers/xrizer" && -d "$d/devices/xreal_ultra" ]]; then
      printf '%s\n' "$d"
      return 0
    fi
    d="$(dirname "$d")"
  done
  return 1
}

ROOT_PROJECT="$(expand_path "${ROOT_PROJECT:-${XR_ROOT_PROJECT:-$(find_package_or_project_root || true)}}")"
[[ -n "$ROOT_PROJECT" && -d "$ROOT_PROJECT" ]] || fail "cannot determine ROOT_PROJECT"
XR_BIN_ROOT="$(expand_path "${XR_BIN_ROOT:-$ROOT_PROJECT/bin}")"
XRIZER_RUNTIME_DIR="$(expand_path "${XRIZER_RUNTIME_DIR:-$XR_BIN_ROOT/drivers/xrizer/runtime}")"
OPENVR_PATHS="$(expand_path "${OPENVR_PATHS:-${XDG_CONFIG_HOME:-$HOME/.config}/openvr/openvrpaths.vrpath}")"
MODE="${1:-add}"

case "$MODE" in
  add|--add) MODE="add" ;;
  remove|--remove) MODE="remove" ;;
  show|--show) MODE="show" ;;
  *) fail "usage: $0 [add|remove|show]" ;;
esac

mkdir -p "$(dirname "$OPENVR_PATHS")"

python3 - "$OPENVR_PATHS" "$XRIZER_RUNTIME_DIR" "$MODE" <<'PY'
import json
import sys
from pathlib import Path

path = Path(sys.argv[1])
runtime = str(Path(sys.argv[2]).expanduser())
mode = sys.argv[3]

if path.exists():
    try:
        data = json.loads(path.read_text() or "{}")
    except Exception:
        data = {}
else:
    data = {}

if not isinstance(data, dict):
    data = {}

data.setdefault("version", 1)
runtimes = data.get("runtime")
if not isinstance(runtimes, list):
    runtimes = []

# Keep non-xrizer entries, remove exact duplicates.
seen = set()
clean = []
for item in runtimes:
    if not isinstance(item, str):
        continue
    if item == runtime:
        continue
    if item in seen:
        continue
    seen.add(item)
    clean.append(item)

if mode == "add":
    clean.insert(0, runtime)
elif mode == "remove":
    pass
elif mode == "show":
    pass

data["runtime"] = clean
if mode != "show":
    path.write_text(json.dumps(data, indent=2) + "\n")

print(json.dumps(data, indent=2))
PY

log "openvrpaths: $OPENVR_PATHS"
log "xrizer runtime: $XRIZER_RUNTIME_DIR"
