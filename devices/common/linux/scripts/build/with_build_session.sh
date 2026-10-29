#!/usr/bin/env bash
# Build trees belong to one invocation. Local dependency/compiler caches survive
# cleanup so the same build can run offline after its first successful run.
set -euo pipefail
[[ $# -gt 0 ]] || { echo "usage: $0 command [args...]" >&2; exit 2; }
root="${ROOT_PROJECT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../../.." && pwd)}"
export ROOT_PROJECT="$root"
export XR_KEEP_OPENCV_BUILD="${XR_KEEP_OPENCV_BUILD:-1}"
case "$XR_KEEP_OPENCV_BUILD" in
  0|1) ;;
  *) echo "[build-session][ERROR] XR_KEEP_OPENCV_BUILD must be 0 or 1" >&2; exit 2 ;;
esac
mkdir -p "$root/build/.sessions"
session="$(mktemp -d "$root/build/.sessions/run.XXXXXXXX")"
cleanup() {
  local status=$?
  if [[ "$XR_KEEP_OPENCV_BUILD" == "1" && -d "$session/opencv" ]]; then
    find "$session" -mindepth 1 -maxdepth 1 ! -name opencv -exec rm -rf -- {} +
    echo "[build-session] kept OpenCV build/SDK: $session/opencv (XR_KEEP_OPENCV_BUILD=0 disables retention)" >&2
  else
    rm -rf -- "$session"
  fi
  return "$status"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
export XR_BUILD_SESSION_DIR="$session" XR_BUILD_SESSION_OWNER="$$"
export XR_BUILD_ROOT="$session/build"
export CCACHE_DIR="$session/ccache" CCACHE_TEMPDIR="$session/ccache-tmp"
export CCACHE_CONFIGPATH="$session/ccache.conf"
export CCACHE_SECONDARY_STORAGE="" CCACHE_REMOTE_STORAGE=""
export XR_CPU_PROFILE_FILE="$session/cpu-profile.json"
support_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
profile_values="$(python3 "$support_dir/../runtime/cpu_target.py" \
  describe "${XR_BUILD_CPU:-avx2}" --output "$XR_CPU_PROFILE_FILE")"
IFS=$'\t' read -r XR_BUILD_CPU XR_CPU_TARGET XR_CPU_TUNE XR_OPENCV_CPU_BASELINE XR_CPU_CACHE_TAG <<< "$profile_values"
export XR_BUILD_CPU XR_CPU_TARGET XR_CPU_TUNE XR_OPENCV_CPU_BASELINE XR_CPU_CACHE_TAG
export CFLAGS="${CFLAGS:-} -march=$XR_CPU_TARGET -mtune=$XR_CPU_TUNE -g0"
export CXXFLAGS="${CXXFLAGS:-} -march=$XR_CPU_TARGET -mtune=$XR_CPU_TUNE -g0"
export CXXFLAGS_EXTRA="${CXXFLAGS_EXTRA:-} -march=$XR_CPU_TARGET -mtune=$XR_CPU_TUNE -g0"
export XR_UPDATE_DEPS="${XR_UPDATE_DEPS:-0}"
case "$XR_UPDATE_DEPS:${XR_OFFLINE:-auto}" in
  0:auto|0:0|0:1|1:auto|1:0) ;;
  *) echo "[dependencies][ERROR] expected XR_UPDATE_DEPS=0|1, XR_OFFLINE=auto|0|1; update requires online mode" >&2; exit 2 ;;
esac
if [[ -n "${GITHUB_RUN_ID:-}${XR_LOCAL_BUILD_ID:-}" ]]; then
  # Preserve the isolated CI/act workflow and its run/attempt SDK checks.
  export XR_DEPENDENCY_CACHE_ROOT="$session/cache"
  offline_default=0
else
  export XR_DEPENDENCY_CACHE_ROOT="${XR_DEPENDENCY_CACHE_ROOT:-$root/build/.dependency-cache}"
  offline_default=auto
fi
mkdir -p "$XR_DEPENDENCY_CACHE_ROOT"
export XR_DEPENDENCY_CACHE_ROOT="$(cd "$XR_DEPENDENCY_CACHE_ROOT" && pwd)"
state_key="$(python3 "$support_dir/dependency_state.py" "$root" "$@")"
ready="$XR_DEPENDENCY_CACHE_ROOT/ready/$state_key"
if [[ "$XR_UPDATE_DEPS" == 1 ]]; then
  export XR_OFFLINE=0
elif [[ "${XR_OFFLINE:-auto}" == auto ]]; then
  export XR_OFFLINE=0
  [[ "$offline_default" != auto || ! -f "$ready" ]] || export XR_OFFLINE=1
fi
source "$support_dir/dependency_policy.sh"
export CCACHE_DIR="$XR_DEPENDENCY_CACHE_ROOT/ccache"
export VCPKG_DEFAULT_BINARY_CACHE="$XR_DEPENDENCY_CACHE_ROOT/vcpkg-binaries"
export VCPKG_DOWNLOADS="$XR_DEPENDENCY_CACHE_ROOT/downloads/vcpkg"
export VCPKG_BINARY_SOURCES="clear;files,$VCPKG_DEFAULT_BINARY_CACHE,readwrite"
export XDG_CACHE_HOME="$XR_DEPENDENCY_CACHE_ROOT/xdg"
export OPENCV_DOWNLOAD_PATH="$XR_DEPENDENCY_CACHE_ROOT/downloads/opencv"
export CARGO_CACHE_AUTO_CLEAN_FREQUENCY=never
mkdir -p "$CCACHE_DIR" "$CCACHE_TEMPDIR" "$VCPKG_DEFAULT_BINARY_CACHE" "$VCPKG_DOWNLOADS" "$XDG_CACHE_HOME" "$OPENCV_DOWNLOAD_PATH"
printf '%s\n' 'max_size = 2G' 'remote_storage =' > "$CCACHE_CONFIGPATH"
export CLEAN_BUILD=1
export CMAKE_BUILD_TYPE=Release BUILD_TYPE=Release
export VCPKG_MAX_CONCURRENCY="${BUILD_JOBS:-2}"
export VCPKG_TRIPLET="x64-linux-$XR_BUILD_CPU-release"
export XR_VCPKG_INSTALLED_DIR="$XR_DEPENDENCY_CACHE_ROOT/vcpkg-installed/$XR_CPU_CACHE_TAG"
export BASALT_VCPKG_INSTALLED="${BASALT_VCPKG_INSTALLED:-$XR_VCPKG_INSTALLED_DIR/$VCPKG_TRIPLET}"
if command -v ccache >/dev/null 2>&1; then
  export CMAKE_C_COMPILER_LAUNCHER=ccache CMAKE_CXX_COMPILER_LAUNCHER=ccache
fi
# CI dependency artifacts are accepted only from this workflow run AND attempt.
# Locally the random session identity prevents reuse even after an interrupted run.
if [[ -n "${XR_LOCAL_BUILD_ID:-}" ]]; then
  export XR_BUILD_DEPENDENCY_TOKEN="act:$XR_LOCAL_BUILD_ID"
elif [[ -n "${GITHUB_RUN_ID:-}" && -n "${GITHUB_RUN_ATTEMPT:-}" ]]; then
  export XR_BUILD_DEPENDENCY_TOKEN="ci:$GITHUB_RUN_ID:$GITHUB_RUN_ATTEMPT"
else
  export XR_BUILD_DEPENDENCY_TOKEN="local:${session##*/}"
fi
echo "[build-session] fresh build: $session ($XR_BUILD_CPU / $XR_CPU_TARGET; offline=$XR_OFFLINE, update=$XR_UPDATE_DEPS)" >&2
echo "[build-session] dependency cache: $XR_DEPENDENCY_CACHE_ROOT" >&2
if [[ "$XR_OFFLINE" == 1 ]]; then
  if python3 "$support_dir/offline_exec.py" "$@"; then status=0; else status=$?; fi
else
  # A failed refresh may leave only part of the dependencies updated.
  rm -f -- "$ready"
  if "$@"; then status=0; else status=$?; fi
fi
if [[ "$status" == 0 ]]; then
  mkdir -p "$(dirname "$ready")"
  printf '%s\n' 'Dependencies prepared by a successful build.' > "$ready"
elif [[ "$XR_OFFLINE" == 1 ]]; then
  echo "[dependencies] If a cached dependency is missing, run the same build with XR_OFFLINE=0. To refresh existing dependencies use XR_UPDATE_DEPS=1." >&2
fi
exit "$status"
