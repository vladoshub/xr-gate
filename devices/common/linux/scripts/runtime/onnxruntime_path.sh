#!/usr/bin/env bash

xr_onnxruntime_arch() {
  case "$(uname -m)" in
    aarch64|arm64) printf 'aarch64\n' ;;
    riscv64) printf 'riscv64\n' ;;
    *) printf 'x64\n' ;;
  esac
}

xr_onnxruntime_version() {
  local requested_version="${XR_ONNXRUNTIME_VERSION:-1.31.0}"
  case "$requested_version" in
    1.31.0|1.18.1) printf '%s\n' "$requested_version" ;;
    *)
      printf '[onnxruntime][ERROR] XR_ONNXRUNTIME_VERSION must be 1.31.0 or 1.18.1, got %s\n' "$requested_version" >&2
      return 2
      ;;
  esac
}

xr_onnxruntime_package_name() {
  local package_arch="${1:-$(xr_onnxruntime_arch)}"
  local package_version
  package_version="${2:-$(xr_onnxruntime_version)}" || return
  printf 'onnxruntime-linux-%s-%s\n' "$package_arch" "$package_version"
}

xr_onnxruntime_mercury_root() {
  local bin_root="$1" mercury_dir="$2"
  local package_arch sdk_dir selected current legacy
  package_arch="$(xr_onnxruntime_arch)"
  sdk_dir="$bin_root/onnxruntime"
  current="$(xr_onnxruntime_package_name "$package_arch" 1.31.0)"
  legacy="$(xr_onnxruntime_package_name "$package_arch" 1.18.1)"

  if [[ -f "$mercury_dir/ONNXRUNTIME_PACKAGE" ]]; then
    selected="$(<"$mercury_dir/ONNXRUNTIME_PACKAGE")"
    case "$selected" in
      "$current"|"$legacy") ;;
      *)
        printf '[onnxruntime][ERROR] Invalid ONNXRUNTIME_PACKAGE in %s\n' "$mercury_dir" >&2
        return 2
        ;;
    esac
  else
    selected="$current"
    # A pre-selection Mercury binary on AMD64/ARM64 uses the legacy SDK.
    # This also covers a partial update that already installed the newer SDK for FRAME.
    if [[ -f "$sdk_dir/$legacy/lib/libonnxruntime.so" ]] &&
       [[ ! -f "$sdk_dir/$current/lib/libonnxruntime.so" ||
          ( "$package_arch" != riscv64 && -f "$mercury_dir/libxr_mercury_runtime.so" ) ]]; then
      selected="$legacy"
    fi
  fi
  printf '%s/%s\n' "$sdk_dir" "$selected"
}
