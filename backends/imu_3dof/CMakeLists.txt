cmake_minimum_required(VERSION 3.16)

project(xr_imu_3dof LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

get_filename_component(XR_TRACKING_ROOT_DEFAULT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(XR_TRACKING_ROOT "${XR_TRACKING_ROOT_DEFAULT}" CACHE PATH "Path to xr_tracking repository")
set(XR_SHARED_INCLUDE_DIR "${XR_TRACKING_ROOT}/shared/include" CACHE PATH "Path to shared/include")

set(VCPKG_INSTALLED "" CACHE PATH "Optional vcpkg_installed/<triplet> prefix")
if(VCPKG_INSTALLED AND IS_DIRECTORY "${VCPKG_INSTALLED}")
  list(PREPEND CMAKE_PREFIX_PATH "${VCPKG_INSTALLED}")
endif()

if(NOT IS_DIRECTORY "${XR_SHARED_INCLUDE_DIR}")
  message(FATAL_ERROR "shared/include not found: ${XR_SHARED_INCLUDE_DIR}")
endif()
if(NOT IS_DIRECTORY "${XR_SHARED_INCLUDE_DIR}/capture_client")
  message(FATAL_ERROR "capture_client headers not found: ${XR_SHARED_INCLUDE_DIR}/capture_client")
endif()
if(NOT IS_DIRECTORY "${XR_SHARED_INCLUDE_DIR}/xr_tracking")
  message(FATAL_ERROR "xr_tracking headers not found: ${XR_SHARED_INCLUDE_DIR}/xr_tracking")
endif()

find_package(Threads REQUIRED)

find_package(CLI11 CONFIG QUIET)
if(TARGET CLI11::CLI11)
  set(XR_CLI11_TARGET CLI11::CLI11)
else()
  find_path(CLI11_INCLUDE_DIR NAMES CLI/CLI.hpp)
  if(NOT CLI11_INCLUDE_DIR)
    message(FATAL_ERROR
      "CLI11 headers not found. Install system package:\n"
      "  sudo apt install libcli11-dev\n"
      "or pass -DCMAKE_PREFIX_PATH=/path/to/deps")
  endif()
  add_library(xr_cli11 INTERFACE)
  target_include_directories(xr_cli11 INTERFACE "${CLI11_INCLUDE_DIR}")
  set(XR_CLI11_TARGET xr_cli11)
endif()

find_package(nlohmann_json CONFIG QUIET)
if(TARGET nlohmann_json::nlohmann_json)
  set(XR_JSON_TARGET nlohmann_json::nlohmann_json)
else()
  find_path(NLOHMANN_JSON_INCLUDE_DIR NAMES nlohmann/json.hpp)
  if(NOT NLOHMANN_JSON_INCLUDE_DIR)
    message(FATAL_ERROR
      "nlohmann/json headers not found. Install system package:\n"
      "  sudo apt install nlohmann-json3-dev\n"
      "or pass -DCMAKE_PREFIX_PATH=/path/to/deps")
  endif()
  add_library(xr_nlohmann_json INTERFACE)
  target_include_directories(xr_nlohmann_json INTERFACE "${NLOHMANN_JSON_INCLUDE_DIR}")
  set(XR_JSON_TARGET xr_nlohmann_json)
endif()

add_executable(imu_3dof_backend
  src/imu_3dof_backend.cpp
)

target_include_directories(imu_3dof_backend PRIVATE
  "${XR_SHARED_INCLUDE_DIR}"
)

target_link_libraries(imu_3dof_backend PRIVATE
  Threads::Threads
  ${XR_CLI11_TARGET}
  ${XR_JSON_TARGET}
)

target_compile_options(imu_3dof_backend PRIVATE
  -Wall
  -Wextra
  -Wpedantic
)

if(UNIX AND NOT APPLE)
  target_link_libraries(imu_3dof_backend PRIVATE rt)
endif()

install(TARGETS imu_3dof_backend
  RUNTIME DESTINATION bin/backends/imu_3dof
)
