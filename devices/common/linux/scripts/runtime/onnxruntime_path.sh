#!/usr/bin/env bash

xr_onnxruntime_arch() {
  case "$(uname -m)" in
    aarch64|arm64) printf 'aarch64\n' ;;
    riscv64) printf 'riscv64\n' ;;
    *) printf 'x64\n' ;;
  esac
}

xr_onnxruntime_version() {
  case "$1" in
    riscv64) printf '1.31.0\n' ;;
    *) printf '1.18.1\n' ;;
  esac
}

xr_onnxruntime_package_name() {
  local package_arch="${1:-$(xr_onnxruntime_arch)}"
  printf 'onnxruntime-linux-%s-%s\n' "$package_arch" "$(xr_onnxruntime_version "$package_arch")"
}
