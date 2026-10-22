cmake_minimum_required(VERSION 3.20)

project(xr_tracking_openvr_driver LANGUAGES CXX)

if(WIN32 AND NOT MSVC)
  message(FATAL_ERROR "The Windows OpenVR driver requires the MSVC C++ ABI. Use MSVC/clang-cl or the existing Windows CI job; MinGW C++ interface calls are not supported.")
endif()

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
option(XR_OPENVR_BUILD_TESTS "Build OpenVR driver functional tests" OFF)
set(NLOHMANN_JSON_INCLUDE_DIR "" CACHE PATH "Directory containing nlohmann/json.hpp (Windows TCP/tests only)")

if(WIN32 OR XR_OPENVR_BUILD_TESTS)
  find_package(Threads REQUIRED)
  find_package(nlohmann_json 3.11 CONFIG QUIET)
  add_library(xr_openvr_tcp STATIC src/runtime_tcp_client.cpp src/runtime_tcp_readers.cpp)
  set_target_properties(xr_openvr_tcp PROPERTIES POSITION_INDEPENDENT_CODE ON)
  target_compile_features(xr_openvr_tcp PUBLIC cxx_std_17)
  target_include_directories(xr_openvr_tcp PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src" "${XR_TRACKING_ROOT}/shared/include")
  target_link_libraries(xr_openvr_tcp PUBLIC Threads::Threads)
  if(TARGET nlohmann_json::nlohmann_json)
    target_link_libraries(xr_openvr_tcp PUBLIC nlohmann_json::nlohmann_json)
  elseif(EXISTS "${NLOHMANN_JSON_INCLUDE_DIR}/nlohmann/json.hpp")
    target_include_directories(xr_openvr_tcp PUBLIC "${NLOHMANN_JSON_INCLUDE_DIR}")
  else()
    message(FATAL_ERROR "Windows TCP/tests require nlohmann_json >=3.11 or NLOHMANN_JSON_INCLUDE_DIR")
  endif()
  if(WIN32)
    target_link_libraries(xr_openvr_tcp PUBLIC ws2_32)
    target_compile_definitions(xr_openvr_tcp PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN)
  endif()
endif()
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
  target_link_libraries(${DRIVER_NAME} PRIVATE ws2_32 xr_openvr_tcp)
  target_compile_definitions(${DRIVER_NAME} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
  # Keep the SteamVR bin/win64 layout with Visual Studio as well as Ninja.
  foreach(config DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
    set_target_properties(${DRIVER_NAME} PROPERTIES RUNTIME_OUTPUT_DIRECTORY_${config} "${XR_PACKAGE_BIN_DIR}")
  endforeach()
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

if(WIN32 AND EXISTS "${XR_OPENVR_SDK_ROOT}/bin/win64/openvr_api.dll")
  add_custom_command(TARGET ${DRIVER_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${XR_OPENVR_SDK_ROOT}/bin/win64/openvr_api.dll" "${XR_PACKAGE_BIN_DIR}/")
endif()

if(XR_OPENVR_BUILD_TESTS)
  enable_testing()
  find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
  add_executable(openvr_hmd_lifecycle_test tests/hmd_lifecycle_test.cpp)
  target_compile_features(openvr_hmd_lifecycle_test PRIVATE cxx_std_17)
  target_include_directories(openvr_hmd_lifecycle_test PRIVATE "${OPENVR_INCLUDE_DIR}")
  target_link_libraries(openvr_hmd_lifecycle_test PRIVATE ${DRIVER_NAME} xr_openvr_tcp)
  if(WIN32)
    # Keep test executables outside the release package, alongside DLL copies.
    add_custom_command(TARGET openvr_hmd_lifecycle_test POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:${DRIVER_NAME}>" "$<TARGET_FILE_DIR:openvr_hmd_lifecycle_test>/"
      COMMAND ${CMAKE_COMMAND} -E copy_if_different "${XR_OPENVR_SDK_ROOT}/bin/win64/openvr_api.dll" "$<TARGET_FILE_DIR:openvr_hmd_lifecycle_test>/")
  endif()
  add_test(NAME openvr_hmd_lifecycle COMMAND openvr_hmd_lifecycle_test)
  set_tests_properties(openvr_hmd_lifecycle PROPERTIES TIMEOUT 15)
  add_executable(openvr_app_tcp_probe tests/app_tcp_probe.cpp
    src/runtime_pose_reader.cpp src/runtime_hand_reader.cpp src/runtime_controller_state_reader.cpp
    ${XR_RUNTIME_POSE_PLATFORM_SOURCE} ${XR_RUNTIME_HAND_PLATFORM_SOURCE} ${XR_RUNTIME_CONTROLLER_PLATFORM_SOURCE})
  target_link_libraries(openvr_app_tcp_probe PRIVATE xr_openvr_tcp)
  add_test(NAME openvr_app_tcp COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/app_tcp_test.py"
           --probe "$<TARGET_FILE:openvr_app_tcp_probe>")
  set_tests_properties(openvr_app_tcp PROPERTIES TIMEOUT 45)
  if(UNIX AND NOT APPLE)
    add_executable(openvr_posix_shm_test tests/posix_shm_test.cpp
      src/runtime_pose_reader.cpp src/runtime_hand_reader.cpp src/runtime_controller_state_reader.cpp
      src/platform/posix_runtime_pose_reader.cpp src/platform/posix_runtime_hand_reader.cpp
      src/platform/posix_runtime_controller_state_reader.cpp)
    target_compile_features(openvr_posix_shm_test PRIVATE cxx_std_17)
    target_include_directories(openvr_posix_shm_test PRIVATE
      "${CMAKE_CURRENT_SOURCE_DIR}/src" "${XR_TRACKING_ROOT}/shared/include")
    target_link_libraries(openvr_posix_shm_test PRIVATE Threads::Threads rt)
    add_test(NAME openvr_posix_shm COMMAND openvr_posix_shm_test)
    set_tests_properties(openvr_posix_shm PROPERTIES TIMEOUT 15)
  endif()
endif()
