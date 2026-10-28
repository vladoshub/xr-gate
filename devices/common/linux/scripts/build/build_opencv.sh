#!/usr/bin/env bash
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
if [[ -z "${XR_BUILD_SESSION_DIR:-}" || ! -d "$XR_BUILD_SESSION_DIR" ]] || [[ -z "${XR_BUILD_SESSION_OWNER:-}" ]] || ! kill -0 "${XR_BUILD_SESSION_OWNER:-0}" 2>/dev/null; then
  exec bash "$SCRIPT_DIR/with_build_session.sh" bash "$0" "$@"
fi
: "${XR_BUILD_DEPENDENCY_TOKEN:?missing build session identity}"
# Annotated tag 4.10.0, pinned to its resolved commit, not a moving branch.
OPENCV_COMMIT=71d3237a093b60a27601c20e9ee6c3e52154e8b1
prefix="${XR_OPENCV_PREFIX:-$XR_BUILD_SESSION_DIR/opencv/install}"
identity="$prefix/XR_BUILD_ID"
. /etc/os-release
expected="${XR_BUILD_DEPENDENCY_TOKEN}|${ID}:${VERSION_ID}|${OPENCV_COMMIT}|${XR_CPU_CACHE_TAG}"
if [[ -f "$identity" ]]; then
  [[ "$(cat "$identity")" == "$expected" && -f "$prefix/lib/cmake/opencv4/OpenCVConfig.cmake" && -f "$prefix/licenses/OpenCV-LICENSE.txt" ]] || {
    echo "[opencv][ERROR] SDK belongs to another build, attempt, distro or CPU target: $prefix" >&2; exit 2;
  }
  echo "[opencv] reusing SDK from this build only: $prefix" >&2
  exit 0
fi
if [[ -d "$prefix" && -n "$(find "$prefix" -mindepth 1 -maxdepth 1 -print -quit)" ]]; then
  echo "[opencv][ERROR] refusing an existing SDK without current-build identity: $prefix" >&2; exit 2
fi
src="$ROOT_PROJECT/third_party/opencv"
build="$XR_BUILD_SESSION_DIR/opencv/build"
mkdir -p "$prefix"
bash "$SCRIPT_DIR/prepare_opencv_source.sh" "$src" "$OPENCV_COMMIT"
cmake -S "$src" -B "$build" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$prefix" \
  -DCMAKE_INSTALL_LIBDIR=lib -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
  -DCMAKE_C_FLAGS="-march=$XR_CPU_TARGET -mtune=$XR_CPU_TUNE -g0" \
  -DCMAKE_CXX_FLAGS="-march=$XR_CPU_TARGET -mtune=$XR_CPU_TUNE -g0" \
  -DBUILD_SHARED_LIBS=OFF \
  -DBUILD_LIST=core,imgproc,imgcodecs,calib3d,features2d,flann,video,videoio,highgui \
  -DCPU_BASELINE="$XR_OPENCV_CPU_BASELINE" -DCPU_DISPATCH= \
  -DBUILD_TESTS=OFF -DBUILD_PERF_TESTS=OFF -DBUILD_EXAMPLES=OFF -DBUILD_DOCS=OFF \
  -DBUILD_opencv_apps=OFF -DBUILD_JAVA=OFF \
  -DBUILD_opencv_python2=OFF -DBUILD_opencv_python3=OFF \
  -DOPENCV_GENERATE_PKGCONFIG=ON -DOPENCV_ENABLE_NONFREE=OFF \
  -DWITH_V4L=ON -DWITH_LIBV4L=OFF \
  -DWITH_FFMPEG=OFF -DWITH_GSTREAMER=OFF -DWITH_GTK=OFF -DWITH_QT=OFF \
  -DWITH_IPP=OFF -DWITH_ITT=OFF -DWITH_OPENCL=OFF -DWITH_OPENCLAMDBLAS=OFF \
  -DWITH_OPENCLAMDFFT=OFF -DWITH_VULKAN=OFF -DWITH_CUDA=OFF \
  -DWITH_LAPACK=OFF -DWITH_EIGEN=OFF -DWITH_TBB=OFF -DWITH_OPENMP=OFF \
  -DWITH_PROTOBUF=OFF -DWITH_ADE=OFF -DWITH_OPENEXR=OFF \
  -DWITH_TIFF=OFF -DWITH_WEBP=OFF -DWITH_JASPER=OFF -DWITH_OPENJPEG=OFF \
  -DWITH_AVIF=OFF -DWITH_GDAL=OFF -DWITH_GDCM=OFF -DWITH_1394=OFF \
  -DWITH_OPENNI=OFF -DWITH_OPENNI2=OFF -DWITH_REALSENSE=OFF \
  -DWITH_GPHOTO2=OFF -DWITH_ARAVIS=OFF -DWITH_XINE=OFF \
  -DWITH_JPEG=ON -DBUILD_JPEG=ON -DWITH_PNG=ON -DBUILD_PNG=ON -DBUILD_ZLIB=ON
cmake --build "$build" --parallel "${BUILD_JOBS:-2}"
cmake --install "$build" --strip
mkdir -p "$prefix/licenses"
cp "$src/LICENSE" "$prefix/licenses/OpenCV-LICENSE.txt"
cp "$src/COPYRIGHT" "$prefix/licenses/OpenCV-COPYRIGHT.txt"
# Preserve upstream's installed third-party notices for the bundled codecs.
[[ -d "$prefix/share/licenses/opencv4" ]] || {
  echo "[opencv][ERROR] installed third-party license bundle is missing" >&2; exit 2;
}
cp -a "$prefix/share/licenses/opencv4" "$prefix/licenses/third-party"
cat > "$prefix/licenses/SOURCE_INFO.txt" <<EOF
Component: OpenCV 4.10.0 (static, PIC, $XR_BUILD_CPU)
CPU target: $XR_CPU_TARGET; tune: $XR_CPU_TUNE; SDK CPU identity: $XR_CPU_CACHE_TAG
Repository: https://github.com/opencv/opencv
Resolved commit: $OPENCV_COMMIT
Local modification: CMake 4 compatibility for cmake/OpenCVGenPkgconfig.cmake.
License: Apache-2.0; bundled dependencies retain their own notices.
Modules: core imgproc imgcodecs calib3d features2d flann video videoio highgui
Camera backend: Linux UVC/V4L2. GUI and FFmpeg/GStreamer backends disabled.
Nonfree algorithms, IPP, OpenCL, CUDA and external media frameworks disabled.
EOF
git -C "$src" diff -- cmake/OpenCVGenPkgconfig.cmake > "$prefix/licenses/OpenCV-CMake4.patch"
cp "$build/CMakeCache.txt" "$prefix/licenses/BUILD_CONFIGURATION.txt"
cp "$XR_CPU_PROFILE_FILE" "$prefix/licenses/CPU_PROFILE.json"
printf '%s\n' "$expected" > "$identity"
