#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_PROJECT="$(cd "${ROOT_PROJECT:-$SCRIPT_DIR/../../../../..}" && pwd)"
source "$SCRIPT_DIR/dependency_policy.sh"
source "$SCRIPT_DIR/../runtime/onnxruntime_path.sh"

readonly arch="$(xr_onnxruntime_arch)"
version="$(xr_onnxruntime_version)"
readonly version
package="$(xr_onnxruntime_package_name "$arch" "$version")"
if [[ "$version" == 1.31.0 ]]; then
  readonly source_url=https://github.com/microsoft/onnxruntime.git
  # Upstream v1.31.0.
  readonly source_commit=402979ac1bef89dcd55add41eaa7277c407599eb
  source_dir="$ROOT_PROJECT/third_party/onnxruntime-$version"
else
  readonly source_url=https://github.com/ROCm/onnxruntime.git
  readonly source_commit=387127404e6c1d84b3468c387d864877ed1c67fe
  source_dir="$ROOT_PROJECT/third_party/onnxruntime"
fi
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

checkout_pinned_source() {
  local directory="$1" url="$2" commit="$3" ref
  ref="$(xr_ensure_git_source "$directory" "$url" "$commit" 1 "$XR_UPDATE_DEPS" 1)"
  git -C "$directory" checkout --detach "$ref" >&2
  if [[ "$(git -C "$directory" rev-parse HEAD)" != "$commit" ]]; then
    echo "[onnxruntime][ERROR] expected commit $commit in $directory" >&2
    return 2
  fi
  if ! git -C "$directory" diff --quiet HEAD --ignore-submodules=all; then
    echo "[onnxruntime][ERROR] the pinned source checkout has local changes: $directory" >&2
    return 2
  fi
}

if ! complete_sdk "$sdk_dir"; then
  xr_require_network "ONNX Runtime $version source build is missing from the SDK cache"
  checkout_pinned_source "$source_dir" "$source_url" "$source_commit"
  if [[ "$(<"$source_dir/VERSION_NUMBER")" != "$version" ]]; then
    echo "[onnxruntime][ERROR] expected $version at $source_commit" >&2
    exit 2
  fi
  stage="$(mktemp -d "$cache_dir/build.XXXXXXXX")"
  trap 'rm -rf -- "$stage"' EXIT
  git clone --quiet --shared --no-checkout -- "$source_dir" "$stage/source"
  git -C "$stage/source" checkout --quiet --detach "$source_commit"
  build_args=(
    --config Release --build_shared_lib --cmake_generator Ninja
    --parallel "${BUILD_JOBS:-2}" --skip_tests --compile_no_warning_as_error
  )
  cmake_defines=(onnxruntime_BUILD_UNIT_TESTS=OFF CMAKE_POLICY_VERSION_MINIMUM=3.5)
  if [[ "$version" == 1.31.0 ]]; then
    # The upstream packer for 1.31.0 reads a staged install with lib64.
    cmake_defines+=(CMAKE_INSTALL_LIBDIR=lib64)
    build_args+=(--skip_submodule_sync)
    if [[ "$arch" == riscv64 ]]; then
      cmake_defines+=(onnxruntime_USE_RVV=OFF)
    fi
  else
    # Keep the Eigen revision pinned by the legacy release's cmake/deps.txt.
    eigen_dir="$ROOT_PROJECT/third_party/onnxruntime-eigen"
    eigen_commit=e7248b26a1ed53fa030c5c459f7ea095dfd276ac
    checkout_pinned_source "$eigen_dir" https://github.com/eigen-mirror/eigen.git "$eigen_commit"
    build_args+=(--use_preinstalled_eigen --eigen_path "$eigen_dir")
    # Backport ONNX Runtime #24583 to the temporary legacy source tree.
    git -C "$stage/source" apply <<'ORT_GCC15_PATCH'
diff --git a/onnxruntime/core/optimizer/transpose_optimization/optimizer_api.h b/onnxruntime/core/optimizer/transpose_optimization/optimizer_api.h
--- a/onnxruntime/core/optimizer/transpose_optimization/optimizer_api.h
+++ b/onnxruntime/core/optimizer/transpose_optimization/optimizer_api.h
@@ -10,4 +10,5 @@
 #include <string_view>
 #include <unordered_map>
 #include <unordered_set>
+#include <cstdint>
 #include <vector>
ORT_GCC15_PATCH
  fi
  build_args+=(--cmake_extra_defines "${cmake_defines[@]}")
  if (( EUID == 0 )); then
    build_args+=(--allow_running_as_root)
  fi
  echo "[onnxruntime] building $version at $source_commit for $arch" >&2
  (
    cd "$stage"
    python3 source/tools/ci_build/build.py --build_dir "$stage" "${build_args[@]}"
    if [[ "$version" == 1.31.0 ]]; then
      DESTDIR="$stage/installed" cmake --install Release --prefix /usr/local
    fi
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
