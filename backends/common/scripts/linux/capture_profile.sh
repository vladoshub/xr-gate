#!/usr/bin/env bash
# Shared capture-profile resolver for backend launchers.
# Source this file; do not execute it directly.

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

capture_profile_resolve_name() {
  local explicit_profile="${1:-}"
  local registry_path="${2:-/tmp/capture_service_streams.json}"
  local fallback_profile="${3:-xreal_air2ultra_unified_480}"
  local profile="$explicit_profile"

  if [[ -z "$profile" ]]; then
    profile="$(capture_profile_read_registry "$registry_path")"
  fi
  if [[ -z "$profile" ]]; then
    profile="$fallback_profile"
  fi

  capture_profile_validate_name "$profile"
  printf '%s\n' "$profile"
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

  CAPTURE_PROFILE_RESOLVED="$(capture_profile_resolve_name \
    "$explicit_profile" "$registry_path" "$fallback_profile")"

  if ! CAPTURE_PROFILE_FILE_RESOLVED="$(capture_profile_find_file \
      "$backend_name" "$CAPTURE_PROFILE_RESOLVED" "$root_project" \
      "$caller_script_dir" "$explicit_dir")"; then
    echo "[capture_profile][ERROR] profile file not found for backend '$backend_name': ${CAPTURE_PROFILE_RESOLVED}.env" >&2
    echo "[capture_profile][ERROR] registry: $registry_path" >&2
    echo "[capture_profile][ERROR] expected under backends/$backend_name/configs/profiles/ or bin/backends/$backend_name/configs/profiles/" >&2
    return 2
  fi

  # shellcheck source=/dev/null
  source "$CAPTURE_PROFILE_FILE_RESOLVED"
  export CAPTURE_PROFILE_RESOLVED CAPTURE_PROFILE_FILE_RESOLVED
}
