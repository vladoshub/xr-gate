#!/usr/bin/env bash

XR_ONNXRUNTIME_VERSION=1.18.1

xr_onnxruntime_package_name() {
  local arch=x64
  case "$(uname -m)" in
    aarch64|arm64) arch=aarch64 ;;
  esac
  printf 'onnxruntime-linux-%s-%s\n' "$arch" "$XR_ONNXRUNTIME_VERSION"
}
