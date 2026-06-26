cmake_minimum_required(VERSION 3.16)

project(override_controller LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

get_filename_component(_OVERRIDE_CONTROLLER_DIR "${CMAKE_CURRENT_LIST_DIR}" ABSOLUTE)
get_filename_component(_XR_TRACKING_ROOT "${_OVERRIDE_CONTROLLER_DIR}/.." ABSOLUTE)

set(XR_TRACKING_ROOT "${_XR_TRACKING_ROOT}" CACHE PATH "Path to xr_tracking repository")
set(XR_SHARED_INCLUDE_DIR "${XR_TRACKING_ROOT}/shared/include" CACHE PATH "Path to shared include directory")

if(NOT EXISTS "${XR_SHARED_INCLUDE_DIR}/xr_runtime/contracts/controller_input_contract.hpp")
  message(FATAL_ERROR "XR shared headers not found: ${XR_SHARED_INCLUDE_DIR}")
endif()

add_executable(override_controller
  src/main.cpp
  src/core/types.cpp
  src/core/config_io.cpp
  src/core/controller_input_publisher.cpp
  src/platform/input_provider.cpp
)

if(WIN32)
  target_sources(override_controller PRIVATE
    src/platform/windows/windows_input_provider.cpp
  )
elseif(UNIX AND NOT APPLE)
  target_sources(override_controller PRIVATE
    src/platform/linux/linux_evdev_input_provider.cpp
  )
endif()

target_include_directories(override_controller PRIVATE
  "${CMAKE_CURRENT_LIST_DIR}/include"
  "${CMAKE_CURRENT_LIST_DIR}/src/platform"
  "${XR_SHARED_INCLUDE_DIR}"
)

find_package(Threads REQUIRED)
target_link_libraries(override_controller PRIVATE Threads::Threads)

find_package(nlohmann_json CONFIG QUIET)
if(nlohmann_json_FOUND)
  target_link_libraries(override_controller PRIVATE nlohmann_json::nlohmann_json)
else()
  find_path(NLOHMANN_JSON_INCLUDE_DIR
    NAMES nlohmann/json.hpp
    HINTS
      "${XR_TRACKING_ROOT}/build/vcpkg_installed/x64-windows/include"
      "${XR_TRACKING_ROOT}/vcpkg_installed/x64-windows/include"
      "${XR_TRACKING_ROOT}/build/backends/basalt_vio/relwithdebinfo/vcpkg_installed/x64-linux/include"
      "${XR_TRACKING_ROOT}/third_party/basalt/build/relwithdebinfo/vcpkg_installed/x64-linux/include"
      "$ENV{HOME}/src/xr_tracking/build/backends/basalt_vio/relwithdebinfo/vcpkg_installed/x64-linux/include"
      "$ENV{HOME}/src/xr_tracking/third_party/basalt/build/relwithdebinfo/vcpkg_installed/x64-linux/include"
      "/usr/local/include"
      "/usr/include"
  )
  if(NOT NLOHMANN_JSON_INCLUDE_DIR)
    message(FATAL_ERROR "nlohmann/json.hpp not found. Install nlohmann-json3-dev or pass -DNLOHMANN_JSON_INCLUDE_DIR=/path/to/include")
  endif()
  target_include_directories(override_controller PRIVATE "${NLOHMANN_JSON_INCLUDE_DIR}")
endif()

if(WIN32)
  target_link_libraries(override_controller PRIVATE ws2_32 user32 xinput9_1_0)
elseif(UNIX AND NOT APPLE)
  target_link_libraries(override_controller PRIVATE rt)
endif()

install(TARGETS override_controller
  RUNTIME DESTINATION bin/override_controller
)
