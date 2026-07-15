#!/usr/bin/env bash
# Shared capture-profile resolver for backend launchers.
# Source this file; do not execute it directly.
#
# Resolution order:
#   1. explicit backend/CAPTURE_PROFILE override passed by the launcher;
#   2. optional capture_tcp_probe metadata lookup;
#   3. local capture registry profile;
#   4. launcher-provided fallback profile.
#
# TCP probing is intentionally disabled by default. Enable/configure it with:
#   CAPTURE_PROFILE_PROBE_ENABLED=1
#   CAPTURE_PROFILE_PROBE_BIN=/path/to/capture_tcp_probe   # optional
#   CAPTURE_PROFILE_PROBE_HOST=127.0.0.1
#   CAPTURE_PROFILE_PROBE_PORT=45660
#   CAPTURE_PROFILE_PROBE_TIMEOUT_MS=1500

capture_profile_expand_tilde() {
  local value="${1:-}"
  case "$value" in
    "~") printf '%s\n' "$HOME" ;;
    "~/"*) printf '%s\n' "$HOME/${value#"~/"}" ;;
    *) printf '%s\n' "$value" ;;
  esac
}

capture_profile_read_registry() {
  local registry_path="${1:-/tmp/capture_service_streams.json}"
  [[ -f "$registry_path" ]] || return 0

  python3 - "$registry_path" <<'PY' 2>/dev/null || true
import json
import sys

try:
    with open(sys.argv[1], "r", encoding="utf-8") as f:
        value = json.load(f).get("profile", "")
    if isinstance(value, str):
        print(value)
except Exception:
    pass
PY
}

capture_profile_validate_name() {
  local profile="${1:-}"
  [[ -n "$profile" ]] || return 1
  [[ "$profile" =~ ^[A-Za-z0-9_.-]+$ ]] || {
    echo "[capture_profile][ERROR] invalid profile name: $profile" >&2
    return 2
  }
}

capture_profile_probe_is_enabled() {
  case "${CAPTURE_PROFILE_PROBE_ENABLED:-0}" in
    1|true|TRUE|yes|YES|on|ON) return 0 ;;
    *) return 1 ;;
  esac
}

# Parse the launcher-only --capture-profile option and preserve all remaining
# arguments for the backend executable. Environment overrides are handled by
# the caller after this function returns.
capture_profile_parse_cli() {
  CAPTURE_PROFILE_CLI_OVERRIDE=""
  CAPTURE_PROFILE_FORWARD_ARGS=()

  while (( $# > 0 )); do
    case "$1" in
      --capture-profile)
        if (( $# < 2 )); then
          echo "[capture_profile][ERROR] --capture-profile requires a value" >&2
          return 2
        fi
        CAPTURE_PROFILE_CLI_OVERRIDE="$2"
        shift 2
        ;;
      --capture-profile=*)
        CAPTURE_PROFILE_CLI_OVERRIDE="${1#*=}"
        shift
        ;;
      --)
        shift
        CAPTURE_PROFILE_FORWARD_ARGS+=("$@")
        break
        ;;
      *)
        CAPTURE_PROFILE_FORWARD_ARGS+=("$1")
        shift
        ;;
    esac
  done

  if [[ -n "$CAPTURE_PROFILE_CLI_OVERRIDE" ]]; then
    capture_profile_validate_name "$CAPTURE_PROFILE_CLI_OVERRIDE"
  fi
  export CAPTURE_PROFILE_CLI_OVERRIDE
}

capture_profile_find_probe_bin() {
  local root_project="${1:-}"
  local explicit="${CAPTURE_PROFILE_PROBE_BIN:-}"
  local candidate=""

  if [[ -n "$explicit" ]]; then
    explicit="$(capture_profile_expand_tilde "$explicit")"
    if [[ -x "$explicit" ]]; then
      printf '%s\n' "$explicit"
      return 0
    fi
    if candidate="$(command -v -- "$explicit" 2>/dev/null)" && [[ -x "$candidate" ]]; then
      printf '%s\n' "$candidate"
      return 0
    fi
    return 1
  fi

  for candidate in \
    "$root_project/bin/capture_service_cpp/capture_tcp_probe" \
    "$root_project/capture_service_cpp/capture_tcp_probe" \
    "$(command -v capture_tcp_probe 2>/dev/null || true)"
  do
    [[ -n "$candidate" && -x "$candidate" ]] && {
      printf '%s\n' "$candidate"
      return 0
    }
  done
  return 1
}

capture_profile_read_tcp_probe() {
  local root_project="${1:-}"
  capture_profile_probe_is_enabled || return 0

  local probe_bin=""
  if ! probe_bin="$(capture_profile_find_probe_bin "$root_project")"; then
    echo "[capture_profile][WARN] TCP profile probe is enabled, but capture_tcp_probe was not found" >&2
    return 0
  fi

  local host="${CAPTURE_PROFILE_PROBE_HOST:-${CAPTURE_TCP_HOST:-${TCP_HOST:-127.0.0.1}}}"
  local port="${CAPTURE_PROFILE_PROBE_PORT:-${CAPTURE_TCP_PORT:-${TCP_PORT:-45660}}}"
  local timeout_ms="${CAPTURE_PROFILE_PROBE_TIMEOUT_MS:-1500}"
  local output=""

  if ! output="$($probe_bin \
      --host "$host" \
      --port "$port" \
      --timeout-ms "$timeout_ms" \
      --print-profile 2>&1)"; then
    echo "[capture_profile][WARN] TCP profile probe failed for $host:$port: $output" >&2
    return 0
  fi

  # Command substitution strips trailing newlines. Reject multi-line or invalid
  # output before it can become a profile filename.
  if [[ "$output" == *$'\n'* ]] || ! capture_profile_validate_name "$output"; then
    echo "[capture_profile][WARN] TCP profile probe returned an invalid profile: $output" >&2
    return 0
  fi
  printf '%s\n' "$output"
}

capture_profile_resolve() {
  local explicit_profile="${1:-}"
  local registry_path="${2:-/tmp/capture_service_streams.json}"
  local fallback_profile="${3:-xreal_air2ultra_unified_480}"
  local root_project="${4:-}"
  local profile=""
  local source=""

  if [[ -n "$explicit_profile" ]]; then
    profile="$explicit_profile"
    source="explicit"
  fi
  if [[ -z "$profile" ]]; then
    profile="$(capture_profile_read_tcp_probe "$root_project")"
    [[ -z "$profile" ]] || source="tcp_probe"
  fi
  if [[ -z "$profile" ]]; then
    profile="$(capture_profile_read_registry "$registry_path")"
    [[ -z "$profile" ]] || source="registry"
  fi
  if [[ -z "$profile" ]]; then
    profile="$fallback_profile"
    source="fallback"
  fi

  capture_profile_validate_name "$profile"
  CAPTURE_PROFILE_RESOLVED="$profile"
  CAPTURE_PROFILE_SOURCE_RESOLVED="$source"
  export CAPTURE_PROFILE_RESOLVED CAPTURE_PROFILE_SOURCE_RESOLVED
}

capture_profile_resolve_name() {
  capture_profile_resolve "${1:-}" "${2:-/tmp/capture_service_streams.json}" \
    "${3:-xreal_air2ultra_unified_480}" "${4:-}"
  printf '%s\n' "$CAPTURE_PROFILE_RESOLVED"
}

capture_profile_find_file() {
  local backend_name="$1"
  local profile_name="$2"
  local root_project="$3"
  local caller_script_dir="$4"
  local explicit_dir="${5:-}"
  local candidate

  if [[ -n "$explicit_dir" ]]; then
    candidate="$explicit_dir/$profile_name.env"
    [[ -f "$candidate" ]] && { printf '%s\n' "$candidate"; return 0; }
  fi

  for candidate in \
    "$caller_script_dir/../../configs/profiles/$profile_name.env" \
    "$root_project/backends/$backend_name/configs/profiles/$profile_name.env" \
    "$root_project/bin/backends/$backend_name/configs/profiles/$profile_name.env"
  do
    [[ -f "$candidate" ]] && { printf '%s\n' "$candidate"; return 0; }
  done

  return 1
}

capture_profile_load_backend() {
  local backend_name="$1"
  local explicit_profile="$2"
  local registry_path="$3"
  local fallback_profile="$4"
  local root_project="$5"
  local caller_script_dir="$6"
  local explicit_dir="${7:-}"

  capture_profile_resolve \
    "$explicit_profile" "$registry_path" "$fallback_profile" "$root_project"

  if ! CAPTURE_PROFILE_FILE_RESOLVED="$(capture_profile_find_file \
      "$backend_name" "$CAPTURE_PROFILE_RESOLVED" "$root_project" \
      "$caller_script_dir" "$explicit_dir")"; then
    echo "[capture_profile][ERROR] profile file not found for backend '$backend_name': ${CAPTURE_PROFILE_RESOLVED}.env" >&2
    echo "[capture_profile][ERROR] profile source: $CAPTURE_PROFILE_SOURCE_RESOLVED" >&2
    echo "[capture_profile][ERROR] registry: $registry_path" >&2
    echo "[capture_profile][ERROR] expected under backends/$backend_name/configs/profiles/ or bin/backends/$backend_name/configs/profiles/" >&2
    return 2
  fi

  # shellcheck source=/dev/null
  source "$CAPTURE_PROFILE_FILE_RESOLVED"
  export CAPTURE_PROFILE_FILE_RESOLVED
}
