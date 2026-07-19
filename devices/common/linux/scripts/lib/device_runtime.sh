#!/usr/bin/env bash
# Shared helpers for hardware-neutral device launch wrappers.

xr_common_log() {
  echo "[device/common] $*" >&2
}

xr_common_fatal() {
  echo "[device/common][ERROR] $*" >&2
  exit 2
}

xr_common_repo_root() {
  local helper_dir
  helper_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
  cd "$helper_dir/../../../../.." && pwd
}

xr_resolve_device_env() {
  local root target candidate
  root="${XR_ROOT_PROJECT:-${ROOT_PROJECT:-$(xr_common_repo_root)}}"
  target="${XR_TARGET_DEVICE:-${XR_DEVICE_TARGET:-generic}}"

  if [[ -n "${XR_DEVICE_ENV:-}" ]]; then
    printf '%s\n' "$XR_DEVICE_ENV"
    return 0
  fi

  if [[ -n "${XR_DEVICE_HOME:-}" ]]; then
    for candidate in \
      "$XR_DEVICE_HOME/device.env" \
      "$XR_DEVICE_HOME/$target.env"; do
      if [[ -f "$candidate" ]]; then
        printf '%s\n' "$candidate"
        return 0
      fi
    done
  fi

  for candidate in \
    "$root/devices/$target/device.env" \
    "$root/devices/$target/$target.env"; do
    if [[ -f "$candidate" ]]; then
      printf '%s\n' "$candidate"
      return 0
    fi
  done

  return 1
}

xr_load_device_env() {
  local env_file
  if ! env_file="$(xr_resolve_device_env)"; then
    xr_common_fatal "device env not found; set XR_DEVICE_ENV or XR_DEVICE_HOME"
  fi
  if [[ ! -f "$env_file" ]]; then
    xr_common_fatal "device env not found: $env_file"
  fi
  export XR_DEVICE_ENV="$env_file"
  # shellcheck source=/dev/null
  source "$env_file"
  export ROOT_PROJECT="${ROOT_PROJECT:-$XR_ROOT_PROJECT}"
}

# Load only the hardware-neutral package environment. This is intended for
# consumers of already-published runtime streams (SteamVR overlays/scenes,
# standalone controller input, and similar tools) that do not need camera,
# display, calibration, or tracking settings from a concrete device profile.
xr_load_common_env() {
  local root common_env
  root="${XR_ROOT_PROJECT:-${ROOT_PROJECT:-$(xr_common_repo_root)}}"
  common_env="$root/devices/common/common.env"

  if [[ ! -f "$common_env" ]]; then
    xr_common_fatal "common runtime env not found: $common_env"
  fi

  export XR_ROOT_PROJECT="$root"
  export ROOT_PROJECT="${ROOT_PROJECT:-$root}"
  export XR_PACKAGE_ROOT="${XR_PACKAGE_ROOT:-$root}"
  export XR_TARGET_DEVICE="${XR_TARGET_DEVICE:-generic}"
  export XR_DEVICE_TARGET="${XR_DEVICE_TARGET:-$XR_TARGET_DEVICE}"
  export XR_DEVICE_HOME="${XR_DEVICE_HOME:-$root/devices/common}"

  # shellcheck source=/dev/null
  source "$common_env"
}

# Prefer the selected device profile when one is available, but permit
# hardware-neutral tools to run directly from the packaged runtime.
xr_load_device_env_or_common() {
  if xr_resolve_device_env >/dev/null 2>&1; then
    xr_load_device_env
  else
    xr_load_common_env
    xr_common_log "no device env selected; using hardware-neutral common environment"
  fi
}

xr_exec_runtime_script() {
  local label="$1"
  local packaged_relative="$2"
  local source_relative="$3"
  shift 3

  local script="$XR_BIN_ROOT/$packaged_relative"
  if [[ ! -x "$script" ]]; then
    script="$XR_ROOT_PROJECT/$source_relative"
  fi
  if [[ ! -x "$script" ]]; then
    xr_common_fatal "$label start script not found; tried $XR_BIN_ROOT/$packaged_relative and $XR_ROOT_PROJECT/$source_relative"
  fi
  exec "$script" "$@"
}
