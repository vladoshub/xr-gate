# XR Tracking: Basalt VIO backend targets.
# This CMakeLists is meant to be added from the upstream Basalt build with:
#   add_subdirectory($ENV{XR_TRACKING_ROOT}/backends/basalt_vio ...)
# It intentionally keeps project-owned source outside the Basalt checkout.

cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED XR_TRACKING_ROOT OR XR_TRACKING_ROOT STREQUAL "")
  get_filename_component(XR_TRACKING_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
endif()

if(NOT EXISTS "${XR_TRACKING_ROOT}/shared/include/capture_client")
  message(FATAL_ERROR
    "capture_client headers not found at ${XR_TRACKING_ROOT}/shared/include/capture_client. "
    "Keep shared/include/capture_client in xr_tracking; it is a common transport SDK, not Basalt-specific.")
endif()

if(NOT TARGET basalt)
  message(FATAL_ERROR
    "backends/basalt_vio must be included from the Basalt CMake build after target 'basalt' exists.")
endif()

find_package(OpenCV REQUIRED COMPONENTS core imgproc)
find_package(TBB QUIET)

set(XR_BASALT_CLI_TARGET "")
if(TARGET CLI11::CLI11)
  set(XR_BASALT_CLI_TARGET CLI11::CLI11)
elseif(TARGET basalt::cli11)
  set(XR_BASALT_CLI_TARGET basalt::cli11)
else()
  message(FATAL_ERROR "No CLI11 target found. Expected CLI11::CLI11 or basalt::cli11 from Basalt build.")
endif()

add_executable(capture_basalt_backend
  src/capture_basalt_backend.cpp
)

target_compile_features(capture_basalt_backend PRIVATE cxx_std_17)

target_include_directories(capture_basalt_backend PRIVATE
  "${XR_TRACKING_ROOT}/shared/include"
  ${OpenCV_INCLUDE_DIRS}
)

target_link_libraries(capture_basalt_backend PRIVATE
  basalt
  nlohmann_json::nlohmann_json
  ${XR_BASALT_CLI_TARGET}
  ${OpenCV_LIBS}
)

# The project install script copies this build-tree executable into
# bin/backends/basalt_vio and puts libbasalt.so next to it under ./lib.
# Use a relative runtime path so the copied binary does not depend on
# LD_LIBRARY_PATH or an absolute build-tree location.
set_target_properties(capture_basalt_backend PROPERTIES
  BUILD_WITH_INSTALL_RPATH TRUE
  INSTALL_RPATH "\$ORIGIN/lib;\$ORIGIN"
)

if(TARGET TBB::tbb)
  target_link_libraries(capture_basalt_backend PRIVATE TBB::tbb)
endif()

add_executable(xr_dump_vio_config
  tools/xr/xr_dump_vio_config.cpp
)

target_compile_features(xr_dump_vio_config PRIVATE cxx_std_17)
target_include_directories(xr_dump_vio_config PRIVATE
  "${XR_TRACKING_ROOT}/shared/include"
)
target_link_libraries(xr_dump_vio_config PRIVATE basalt)

set_target_properties(xr_dump_vio_config PROPERTIES
  BUILD_WITH_INSTALL_RPATH TRUE
  INSTALL_RPATH "\$ORIGIN/lib;\$ORIGIN"
)
