#!/usr/bin/env bash
# One invocation owns all compiler/package caches. Nothing is restored next time.
set -euo pipefail
[[ $# -gt 0 ]] || { echo "usage: $0 command [args...]" >&2; exit 2; }
root="${ROOT_PROJECT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../../.." && pwd)}"
export ROOT_PROJECT="$root"
mkdir -p "$root/build/.sessions"
session="$(mktemp -d "$root/build/.sessions/run.XXXXXXXX")"
cleanup() { rm -rf -- "$session"; }
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
export XR_BUILD_SESSION_DIR="$session" XR_BUILD_SESSION_OWNER="$$"
export XR_BUILD_ROOT="$session/build"
export CCACHE_DIR="$session/ccache" CCACHE_TEMPDIR="$session/ccache-tmp"
export CCACHE_CONFIGPATH="$session/ccache.conf"
export CCACHE_SECONDARY_STORAGE="" CCACHE_REMOTE_STORAGE=""
export VCPKG_DEFAULT_BINARY_CACHE="$session/vcpkg-cache"
export VCPKG_DOWNLOADS="$session/downloads"
export VCPKG_BINARY_SOURCES="clear;files,$VCPKG_DEFAULT_BINARY_CACHE,readwrite"
export XDG_CACHE_HOME="$session/xdg-cache"
mkdir -p "$CCACHE_DIR" "$CCACHE_TEMPDIR" "$VCPKG_DEFAULT_BINARY_CACHE" "$VCPKG_DOWNLOADS" "$XDG_CACHE_HOME"
printf '%s\n' 'max_size = 2G' 'remote_storage =' > "$CCACHE_CONFIGPATH"
export XR_CPU_PROFILE_FILE="$session/cpu-profile.json"
support_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
profile_values="$(python3 "$support_dir/../runtime/cpu_target.py" \
  describe "${XR_BUILD_CPU:-avx2}" --output "$XR_CPU_PROFILE_FILE")"
IFS=$'\t' read -r XR_BUILD_CPU XR_CPU_TARGET XR_CPU_TUNE XR_OPENCV_CPU_BASELINE XR_CPU_CACHE_TAG <<< "$profile_values"
export XR_BUILD_CPU XR_CPU_TARGET XR_CPU_TUNE XR_OPENCV_CPU_BASELINE XR_CPU_CACHE_TAG
export CFLAGS="${CFLAGS:-} -march=$XR_CPU_TARGET -mtune=$XR_CPU_TUNE -g0"
export CXXFLAGS="${CXXFLAGS:-} -march=$XR_CPU_TARGET -mtune=$XR_CPU_TUNE -g0"
export CXXFLAGS_EXTRA="${CXXFLAGS_EXTRA:-} -march=$XR_CPU_TARGET -mtune=$XR_CPU_TUNE -g0"
export CLEAN_BUILD=1
export CMAKE_BUILD_TYPE=Release BUILD_TYPE=Release
export VCPKG_MAX_CONCURRENCY="${BUILD_JOBS:-2}"
export VCPKG_TRIPLET="x64-linux-$XR_BUILD_CPU-release"
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
echo "[build-session] fresh session: $session ($XR_BUILD_CPU / $XR_CPU_TARGET; no persistent cache)" >&2
"$@"
