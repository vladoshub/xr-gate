cmake_minimum_required(VERSION 3.16)

project(xr_spatial LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

get_filename_component(_XR_SPATIAL_BACKEND_DIR "${CMAKE_CURRENT_LIST_DIR}" ABSOLUTE)
get_filename_component(_XR_TRACKING_ROOT "${_XR_SPATIAL_BACKEND_DIR}/../.." ABSOLUTE)

set(XR_TRACKING_ROOT "${_XR_TRACKING_ROOT}" CACHE PATH "Path to xr_tracking repository")
set(XR_SHARED_INCLUDE_DIR "${XR_TRACKING_ROOT}/shared/include" CACHE PATH "Path to shared include directory")

if(NOT EXISTS "${XR_SHARED_INCLUDE_DIR}/capture_client")
  message(FATAL_ERROR "XR shared headers not found: ${XR_SHARED_INCLUDE_DIR}")
endif()

find_package(Threads REQUIRED)
find_package(OpenCV REQUIRED COMPONENTS core imgproc calib3d imgcodecs)

find_package(Eigen3 QUIET)
if(NOT Eigen3_FOUND)
  find_path(EIGEN3_INCLUDE_DIR
    NAMES Eigen/Core
    HINTS
      "/usr/include/eigen3"
      "/usr/local/include/eigen3")
  if(NOT EIGEN3_INCLUDE_DIR)
    message(FATAL_ERROR "Eigen3 not found. Install libeigen3-dev or pass -DEIGEN3_INCLUDE_DIR=/path/to/eigen3")
  endif()
endif()

find_package(CLI11 CONFIG QUIET)
if(NOT CLI11_FOUND)
  find_path(CLI11_INCLUDE_DIR
    NAMES CLI/CLI.hpp
    HINTS
      "/usr/local/include"
      "/usr/include")
  if(NOT CLI11_INCLUDE_DIR)
    message(FATAL_ERROR "CLI11 not found. Install libcli11-dev or pass -DCLI11_INCLUDE_DIR=/path/to/include")
  endif()
endif()

find_package(nlohmann_json CONFIG QUIET)
if(NOT nlohmann_json_FOUND)
  find_path(NLOHMANN_JSON_INCLUDE_DIR
    NAMES nlohmann/json.hpp
    HINTS
      "/usr/local/include"
      "/usr/include")
  if(NOT NLOHMANN_JSON_INCLUDE_DIR)
    message(FATAL_ERROR "nlohmann/json.hpp not found. Install nlohmann-json3-dev or pass -DNLOHMANN_JSON_INCLUDE_DIR=/path/to/include")
  endif()
endif()

add_executable(xr_spatial_backend
  src/xr_spatial_backend.cpp
  src/calibration.cpp
  src/stereo_depth_processor.cpp
  src/pose_reader.cpp
  src/pointcloud_map.cpp
  src/pointcloud_filter.cpp
  src/spatial_proxy_mesh_builder.cpp
)

target_include_directories(xr_spatial_backend PRIVATE
  "${CMAKE_CURRENT_LIST_DIR}/include"
  "${XR_SHARED_INCLUDE_DIR}"
  ${OpenCV_INCLUDE_DIRS}
)

if(Eigen3_FOUND)
  target_link_libraries(xr_spatial_backend PRIVATE Eigen3::Eigen)
else()
  target_include_directories(xr_spatial_backend PRIVATE "${EIGEN3_INCLUDE_DIR}")
endif()

target_link_libraries(xr_spatial_backend PRIVATE
  Threads::Threads
  ${OpenCV_LIBS}
)

if(CLI11_FOUND)
  target_link_libraries(xr_spatial_backend PRIVATE CLI11::CLI11)
else()
  target_include_directories(xr_spatial_backend PRIVATE "${CLI11_INCLUDE_DIR}")
endif()

if(nlohmann_json_FOUND)
  target_link_libraries(xr_spatial_backend PRIVATE nlohmann_json::nlohmann_json)
else()
  target_include_directories(xr_spatial_backend PRIVATE "${NLOHMANN_JSON_INCLUDE_DIR}")
endif()

if(UNIX AND NOT APPLE)
  target_link_libraries(xr_spatial_backend PRIVATE rt)
endif()
