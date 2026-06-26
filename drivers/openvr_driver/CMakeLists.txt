cmake_minimum_required(VERSION 3.20)

project(xr_tracking_openvr_driver LANGUAGES CXX)

set(DRIVER_BASENAME xr_tracking)
set(DRIVER_NAME "driver_${DRIVER_BASENAME}")

set(XR_OPENVR_SDK_ROOT "$ENV{XR_OPENVR_SDK_ROOT}" CACHE PATH "Path to Valve OpenVR SDK checkout/root")
if(NOT XR_OPENVR_SDK_ROOT AND DEFINED ENV{OPENVR_SDK_ROOT})
  set(XR_OPENVR_SDK_ROOT "$ENV{OPENVR_SDK_ROOT}" CACHE PATH "Path to Valve OpenVR SDK checkout/root" FORCE)
endif()
if(NOT XR_OPENVR_SDK_ROOT AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/openvr/headers/openvr_driver.h")
  set(XR_OPENVR_SDK_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/openvr" CACHE PATH "Path to Valve OpenVR SDK checkout/root" FORCE)
endif()

if(NOT XR_OPENVR_SDK_ROOT)
  message(FATAL_ERROR "Set XR_OPENVR_SDK_ROOT or OPENVR_SDK_ROOT to a Valve OpenVR SDK checkout, e.g. ~/src/openvr")
endif()

set(OPENVR_INCLUDE_DIR "${XR_OPENVR_SDK_ROOT}/headers")
if(NOT EXISTS "${OPENVR_INCLUDE_DIR}/openvr_driver.h")
  message(FATAL_ERROR "openvr_driver.h not found under ${OPENVR_INCLUDE_DIR}")
endif()

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
  set(XR_PLATFORM_BITS 64)
else()
  set(XR_PLATFORM_BITS 32)
endif()

if(WIN32)
  add_compile_definitions(_WIN32)
  set(OPENVR_ARCH_TARGET "win${XR_PLATFORM_BITS}")
  set(XR_RUNTIME_POSE_PLATFORM_SOURCE src/platform/windows_runtime_pose_reader.cpp)
  set(XR_RUNTIME_HAND_PLATFORM_SOURCE src/platform/windows_runtime_hand_reader.cpp)
  set(XR_RUNTIME_CONTROLLER_PLATFORM_SOURCE src/platform/windows_runtime_controller_state_reader.cpp)
elseif(APPLE)
  add_compile_definitions(OSX POSIX)
  set(OPENVR_ARCH_TARGET "osx${XR_PLATFORM_BITS}")
  set(XR_RUNTIME_POSE_PLATFORM_SOURCE src/platform/posix_runtime_pose_reader.cpp)
  set(XR_RUNTIME_HAND_PLATFORM_SOURCE src/platform/posix_runtime_hand_reader.cpp)
  set(XR_RUNTIME_CONTROLLER_PLATFORM_SOURCE src/platform/posix_runtime_controller_state_reader.cpp)
else()
  add_compile_definitions(LINUX POSIX)
  set(OPENVR_ARCH_TARGET "linux${XR_PLATFORM_BITS}")
  set(XR_RUNTIME_POSE_PLATFORM_SOURCE src/platform/posix_runtime_pose_reader.cpp)
  set(XR_RUNTIME_HAND_PLATFORM_SOURCE src/platform/posix_runtime_hand_reader.cpp)
  set(XR_RUNTIME_CONTROLLER_PLATFORM_SOURCE src/platform/posix_runtime_controller_state_reader.cpp)
endif()

find_library(OPENVR_LIBRARIES
  NAMES openvr_api
  PATHS
    "${XR_OPENVR_SDK_ROOT}/bin"
    "${XR_OPENVR_SDK_ROOT}/lib"
  PATH_SUFFIXES
    "${OPENVR_ARCH_TARGET}"
    "linux64" "win64" "osx32"
  NO_DEFAULT_PATH
)

if(NOT OPENVR_LIBRARIES)
  message(FATAL_ERROR "openvr_api library not found under ${XR_OPENVR_SDK_ROOT}/bin or lib for ${OPENVR_ARCH_TARGET}")
endif()

set(XR_TRACKING_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." CACHE PATH "xr_tracking repository root")
set(XR_PACKAGE_DIR "${CMAKE_BINARY_DIR}/${DRIVER_BASENAME}")
set(XR_PACKAGE_BIN_DIR "${XR_PACKAGE_DIR}/bin/${OPENVR_ARCH_TARGET}")

add_library(${DRIVER_NAME} SHARED
  src/driver_xr_tracking.cpp
  src/runtime_pose_reader.cpp
  src/runtime_hand_reader.cpp
  src/runtime_controller_state_reader.cpp
  ${XR_RUNTIME_POSE_PLATFORM_SOURCE}
  ${XR_RUNTIME_HAND_PLATFORM_SOURCE}
  ${XR_RUNTIME_CONTROLLER_PLATFORM_SOURCE}
)

set_target_properties(${DRIVER_NAME} PROPERTIES
  CXX_STANDARD 17
  CXX_STANDARD_REQUIRED ON
  PREFIX ""
  LIBRARY_OUTPUT_DIRECTORY "${XR_PACKAGE_BIN_DIR}"
  RUNTIME_OUTPUT_DIRECTORY "${XR_PACKAGE_BIN_DIR}"
  ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
)

if(UNIX AND NOT APPLE)
  set_target_properties(${DRIVER_NAME} PROPERTIES
    BUILD_RPATH "$ORIGIN"
    INSTALL_RPATH "$ORIGIN"
  )
endif()

target_include_directories(${DRIVER_NAME} PRIVATE
  "${OPENVR_INCLUDE_DIR}"
  "${CMAKE_CURRENT_SOURCE_DIR}/src"
  "${XR_TRACKING_ROOT}/shared/include"
)

target_link_libraries(${DRIVER_NAME} PRIVATE "${OPENVR_LIBRARIES}")
if(WIN32)
  target_link_libraries(${DRIVER_NAME} PRIVATE ws2_32)
elseif(UNIX AND NOT APPLE)
  target_link_libraries(${DRIVER_NAME} PRIVATE rt)
endif()

add_custom_command(TARGET ${DRIVER_NAME} POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory "${XR_PACKAGE_DIR}"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${CMAKE_CURRENT_SOURCE_DIR}/driver.vrdrivermanifest"
          "${XR_PACKAGE_DIR}/driver.vrdrivermanifest"
  COMMAND ${CMAKE_COMMAND} -E copy_directory
          "${CMAKE_CURRENT_SOURCE_DIR}/resources"
          "${XR_PACKAGE_DIR}/resources"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${OPENVR_LIBRARIES}"
          "${XR_PACKAGE_BIN_DIR}/"
  COMMENT "Assembling SteamVR driver package at ${XR_PACKAGE_DIR}"
)

message(STATUS "OpenVR SDK: ${XR_OPENVR_SDK_ROOT}")
message(STATUS "OpenVR architecture target: ${OPENVR_ARCH_TARGET}")
message(STATUS "Driver package output: ${XR_PACKAGE_DIR}")
