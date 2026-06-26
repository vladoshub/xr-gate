#!/usr/bin/env bash
set -euo pipefail

log() { echo "[start_openvr_app_via_monado] $*" >&2; }
fail() { echo "[start_openvr_app_via_monado][ERROR] $*" >&2; exit 1; }

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

usage() {
  cat >&2 <<'EOF_USAGE'
Usage:
  start_openvr_app_via_monado.sh --print-steam-options
  start_openvr_app_via_monado.sh -- <command> [args...]

Environment:
  XRIZER_RUNTIME_DIR    xrizer runtime directory; defaults to bin/drivers/xrizer/runtime
  XR_RUNTIME_JSON       OpenXR runtime manifest; defaults to Monado build manifest when found
  XRIZER_RUST_LOG       optional RUST_LOG value for xrizer

Start Monado first, then use this wrapper for a native OpenVR process or copy
printed options into a Steam game's launch options.
EOF_USAGE
}

ROOT_PROJECT="$(expand_path "${ROOT_PROJECT:-${XR_ROOT_PROJECT:-$(find_package_or_project_root || true)}}")"
[[ -n "$ROOT_PROJECT" && -d "$ROOT_PROJECT" ]] || fail "cannot determine ROOT_PROJECT"
XR_BIN_ROOT="$(expand_path "${XR_BIN_ROOT:-$ROOT_PROJECT/bin}")"

XRIZER_RUNTIME_DIR="$(expand_path "${XRIZER_RUNTIME_DIR:-$XR_BIN_ROOT/drivers/xrizer/runtime}")"
[[ -d "$XRIZER_RUNTIME_DIR" ]] || fail "xrizer runtime dir not found: $XRIZER_RUNTIME_DIR. Run drivers/xrizer/scripts/linux/install_xrizer.sh"

if [[ -z "${XR_RUNTIME_JSON:-}" ]]; then
  candidates=(
    "$ROOT_PROJECT/third_party/monado_driver/build/xr_tracking_relwithdebinfo/openxr_monado-dev.json"
    "$ROOT_PROJECT/bin/drivers/monado_driver/openxr_monado-dev.json"
    "$XR_BIN_ROOT/drivers/monado_driver/openxr_monado-dev.json"
    "/usr/share/openxr/1/openxr_monado.json"
  )
  for c in "${candidates[@]}"; do
    if [[ -f "$c" ]]; then
      export XR_RUNTIME_JSON="$c"
      break
    fi
  done
fi

[[ -n "${XR_RUNTIME_JSON:-}" ]] || fail "XR_RUNTIME_JSON is not set and no Monado runtime manifest was found"
[[ -f "$XR_RUNTIME_JSON" ]] || fail "XR_RUNTIME_JSON does not exist: $XR_RUNTIME_JSON"

export VR_OVERRIDE="$XRIZER_RUNTIME_DIR"
export PRESSURE_VESSEL_IMPORT_OPENXR_1_RUNTIMES="${PRESSURE_VESSEL_IMPORT_OPENXR_1_RUNTIMES:-1}"
if [[ -n "${XDG_RUNTIME_DIR:-}" ]]; then
  monado_ipc="$XDG_RUNTIME_DIR/monado_comp_ipc"
  if [[ -n "${PRESSURE_VESSEL_FILESYSTEMS_RW:-}" ]]; then
    case ":$PRESSURE_VESSEL_FILESYSTEMS_RW:" in
      *":$monado_ipc:"*) ;;
      *) export PRESSURE_VESSEL_FILESYSTEMS_RW="$PRESSURE_VESSEL_FILESYSTEMS_RW:$monado_ipc" ;;
    esac
  else
    export PRESSURE_VESSEL_FILESYSTEMS_RW="$monado_ipc"
  fi
fi
if [[ -n "${XRIZER_RUST_LOG:-}" ]]; then
  export RUST_LOG="$XRIZER_RUST_LOG"
fi

if [[ "${1:-}" == "--print-steam-options" ]]; then
  printf 'XR_RUNTIME_JSON=%q VR_OVERRIDE=%q PRESSURE_VESSEL_IMPORT_OPENXR_1_RUNTIMES=1' "$XR_RUNTIME_JSON" "$VR_OVERRIDE"
  if [[ -n "${PRESSURE_VESSEL_FILESYSTEMS_RW:-}" ]]; then
    printf ' PRESSURE_VESSEL_FILESYSTEMS_RW=%q' "$PRESSURE_VESSEL_FILESYSTEMS_RW"
  fi
  if [[ -n "${XRIZER_RUST_LOG:-}" ]]; then
    printf ' RUST_LOG=%q' "$XRIZER_RUST_LOG"
  fi
  printf ' %s\n' '%command%'
  exit 0
fi

if [[ "${1:-}" == "--help" || "${1:-}" == "-h" ]]; then
  usage
  exit 0
fi

if [[ "${1:-}" == "--" ]]; then
  shift
fi

if [[ $# -eq 0 ]]; then
  usage
  exit 2
fi

log "XR_RUNTIME_JSON=$XR_RUNTIME_JSON"
log "VR_OVERRIDE=$VR_OVERRIDE"
log "PRESSURE_VESSEL_IMPORT_OPENXR_1_RUNTIMES=$PRESSURE_VESSEL_IMPORT_OPENXR_1_RUNTIMES"
if [[ -n "${PRESSURE_VESSEL_FILESYSTEMS_RW:-}" ]]; then
  log "PRESSURE_VESSEL_FILESYSTEMS_RW=$PRESSURE_VESSEL_FILESYSTEMS_RW"
fi

exec "$@"
