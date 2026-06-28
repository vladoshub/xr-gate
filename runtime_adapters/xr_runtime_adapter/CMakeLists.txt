cmake_minimum_required(VERSION 3.16)
project(xr_runtime_adapter LANGUAGES CXX)

set(XR_TRACKING_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." CACHE PATH "XR tracking project root")
set(XR_SHARED_INCLUDE_DIR "${XR_TRACKING_ROOT}/shared/include" CACHE PATH "Shared XR tracking include directory")
set(CLI11_INCLUDE_DIR "" CACHE PATH "Optional CLI11 include directory")
set(NLOHMANN_JSON_INCLUDE_DIR "" CACHE PATH "Optional nlohmann_json include directory")

if(NOT EXISTS "${XR_SHARED_INCLUDE_DIR}")
  message(FATAL_ERROR "XR_SHARED_INCLUDE_DIR not found: ${XR_SHARED_INCLUDE_DIR}")
endif()

find_package(Threads REQUIRED)
find_package(CLI11 CONFIG QUIET)
find_package(nlohmann_json CONFIG QUIET)

add_executable(xr_runtime_adapter
  src/xr_runtime_adapter.cpp
  src/coordinate_util.cpp
  src/gestures.cpp
  src/hmd_pose_stability_filter.cpp
  src/jitter_filter.cpp
  src/body_tracker_stability_filter.cpp
  src/override_controller.cpp
)

target_compile_features(xr_runtime_adapter PRIVATE cxx_std_17)
target_include_directories(xr_runtime_adapter PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/src"
  "${XR_SHARED_INCLUDE_DIR}"
)
target_link_libraries(xr_runtime_adapter PRIVATE Threads::Threads)

if(TARGET CLI11::CLI11)
  target_link_libraries(xr_runtime_adapter PRIVATE CLI11::CLI11)
elseif(CLI11_INCLUDE_DIR)
  target_include_directories(xr_runtime_adapter PRIVATE "${CLI11_INCLUDE_DIR}")
else()
  message(FATAL_ERROR "CLI11 not found. Provide CLI11 via CMAKE_PREFIX_PATH or set CLI11_INCLUDE_DIR.")
endif()

if(TARGET nlohmann_json::nlohmann_json)
  target_link_libraries(xr_runtime_adapter PRIVATE nlohmann_json::nlohmann_json)
elseif(NLOHMANN_JSON_INCLUDE_DIR)
  target_include_directories(xr_runtime_adapter PRIVATE "${NLOHMANN_JSON_INCLUDE_DIR}")
else()
  message(FATAL_ERROR "nlohmann_json not found. Provide nlohmann_json via CMAKE_PREFIX_PATH or set NLOHMANN_JSON_INCLUDE_DIR.")
endif()

if(MSVC)
  target_compile_options(xr_runtime_adapter PRIVATE /W4 /permissive-)
  target_compile_definitions(xr_runtime_adapter PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
else()
  target_compile_options(xr_runtime_adapter PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

install(TARGETS xr_runtime_adapter RUNTIME DESTINATION bin/runtime_adapters/xr_runtime_adapter)
