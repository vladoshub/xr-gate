#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_PROJECT="$(cd "${ROOT_PROJECT:-$SCRIPT_DIR/../../../../..}" && pwd)"
source "$SCRIPT_DIR/dependency_policy.sh"

readonly version=1.18.1
readonly source_url=https://github.com/ROCm/onnxruntime.git
readonly source_commit=387127404e6c1d84b3468c387d864877ed1c67fe
package="onnxruntime-linux-x64-$version"
source_dir="$ROOT_PROJECT/third_party/onnxruntime"
install_dir="${XR_BIN_ROOT:-$ROOT_PROJECT/bin}/onnxruntime/$package"
cache_dir="${XR_DEPENDENCY_CACHE_ROOT:-$ROOT_PROJECT/build/.dependency-cache}/onnxruntime/$source_commit"

# Match the SDK to the distro, CPU profile, toolchain flags and build recipe.
build_key="$( {
  sha256sum < "$SCRIPT_DIR/build_onnxruntime.sh"
  cat /etc/os-release
  printf '%s\0' "$(uname -m)" "${XR_CPU_CACHE_TAG:-}" "${CC:-}" "${CXX:-}" \
    "${CFLAGS:-}" "${CXXFLAGS:-}" "${LDFLAGS:-}"
} | sha256sum)"
mkdir -p "$cache_dir"
cache_dir="$(cd "$cache_dir" && pwd)"
sdk_dir="$cache_dir/${build_key%% *}"
exec 9>"$cache_dir/.lock"
flock 9

complete_sdk() {
  local sdk="$1"
  [[ -s "$sdk/include/onnxruntime_c_api.h" && -s "$sdk/include/onnxruntime_cxx_api.h" &&
     -s "$sdk/lib/libonnxruntime.so" && -s "$sdk/lib/libonnxruntime.so.$version" &&
     -s "$sdk/LICENSE" && -s "$sdk/ThirdPartyNotices.txt" &&
     -s "$sdk/GIT_COMMIT_ID" && "$(<"$sdk/GIT_COMMIT_ID")" == "$source_commit" ]]
}

if ! complete_sdk "$sdk_dir"; then
  xr_require_network "ONNX Runtime $version source build is missing from the SDK cache"
  ref="$(xr_ensure_git_source "$source_dir" "$source_url" "$source_commit" 1 "$XR_UPDATE_DEPS" 1)"
  git -C "$source_dir" checkout --detach "$ref" >&2
  if [[ "$(git -C "$source_dir" rev-parse HEAD)" != "$source_commit" ||
        "$(<"$source_dir/VERSION_NUMBER")" != "$version" ]]; then
    echo "[onnxruntime][ERROR] expected $version at $source_commit" >&2
    exit 2
  fi
  if ! git -C "$source_dir" diff --quiet HEAD --ignore-submodules=all; then
    echo "[onnxruntime][ERROR] the pinned source checkout has local changes: $source_dir" >&2
    exit 2
  fi

  stage="$(mktemp -d "$cache_dir/build.XXXXXXXX")"
  trap 'rm -rf -- "$stage"' EXIT
  ln -s "$source_dir" "$stage/source"
  build_args=(
    --config Release --build_shared_lib --cmake_generator Ninja
    --parallel "${BUILD_JOBS:-2}" --skip_tests --compile_no_warning_as_error
    --cmake_extra_defines onnxruntime_BUILD_UNIT_TESTS=OFF CMAKE_POLICY_VERSION_MINIMUM=3.5
  )
  if (( EUID == 0 )); then
    build_args+=(--allow_running_as_root)
  fi
  echo "[onnxruntime] building $version from $source_url at $source_commit" >&2
  (
    cd "$stage"
    python3 source/tools/ci_build/build.py --build_dir "$stage" "${build_args[@]}"
    # The upstream packer expects paths without spaces, so pass relative paths.
    bash source/tools/ci_build/github/linux/copy_strip_binary.sh \
      -r . -s source -a "$package" -l "libonnxruntime.so.$version" -c Release -t "$source_commit"
  ) >&2
  if ! complete_sdk "$stage/$package"; then
    echo "[onnxruntime][ERROR] incomplete SDK at $stage/$package" >&2
    exit 2
  fi
  rm -rf -- "$sdk_dir"
  mv -T -- "$stage/$package" "$sdk_dir"
else
  echo "[onnxruntime] reusing SDK built from $source_commit" >&2
fi

mkdir -p "$install_dir"
rsync -a --delete "$sdk_dir/" "$install_dir/"
printf '%s\n' "$install_dir"
