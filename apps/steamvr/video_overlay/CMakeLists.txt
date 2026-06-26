cmake_minimum_required(VERSION 3.16)

project(xr_steamvr_video_overlay LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

get_filename_component(_XR_VIDEO_OVERLAY_DIR "${CMAKE_CURRENT_LIST_DIR}" ABSOLUTE)
get_filename_component(_XR_TRACKING_ROOT "${_XR_VIDEO_OVERLAY_DIR}/../../.." ABSOLUTE)

set(XR_TRACKING_ROOT "${_XR_TRACKING_ROOT}" CACHE PATH "Path to xr_tracking repository")
set(XR_SHARED_INCLUDE_DIR "${XR_TRACKING_ROOT}/shared/include" CACHE PATH "Path to shared include directory")
set(XR_OPENVR_SDK_ROOT "$ENV{XR_OPENVR_SDK_ROOT}" CACHE PATH "Path to Valve OpenVR SDK checkout/root")

if(NOT XR_OPENVR_SDK_ROOT AND DEFINED ENV{OPENVR_SDK_ROOT})
  set(XR_OPENVR_SDK_ROOT "$ENV{OPENVR_SDK_ROOT}" CACHE PATH "Path to Valve OpenVR SDK checkout/root" FORCE)
endif()

if(NOT XR_OPENVR_SDK_ROOT AND EXISTS "${XR_TRACKING_ROOT}/third_party/openvr/headers/openvr.h")
  set(XR_OPENVR_SDK_ROOT "${XR_TRACKING_ROOT}/third_party/openvr" CACHE PATH "Path to Valve OpenVR SDK checkout/root" FORCE)
endif()

if(NOT XR_OPENVR_SDK_ROOT AND EXISTS "${XR_TRACKING_ROOT}/third-party/openvr/headers/openvr.h")
  set(XR_OPENVR_SDK_ROOT "${XR_TRACKING_ROOT}/third-party/openvr" CACHE PATH "Path to Valve OpenVR SDK checkout/root" FORCE)
endif()

if(NOT XR_OPENVR_SDK_ROOT)
  message(FATAL_ERROR "Set XR_OPENVR_SDK_ROOT or OPENVR_SDK_ROOT to a Valve OpenVR SDK checkout/root")
endif()

set(OPENVR_INCLUDE_DIR "${XR_OPENVR_SDK_ROOT}/headers")
if(NOT EXISTS "${OPENVR_INCLUDE_DIR}/openvr.h")
  message(FATAL_ERROR "openvr.h not found under ${OPENVR_INCLUDE_DIR}")
endif()

if(NOT EXISTS "${XR_SHARED_INCLUDE_DIR}/xr_video/shm/stereo_video_shm_reader.hpp")
  message(FATAL_ERROR "XR video shared headers not found: ${XR_SHARED_INCLUDE_DIR}")
endif()

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
  set(XR_PLATFORM_BITS 64)
else()
  set(XR_PLATFORM_BITS 32)
endif()

if(WIN32)
  set(OPENVR_ARCH_TARGET "win${XR_PLATFORM_BITS}")
elseif(APPLE)
  set(OPENVR_ARCH_TARGET "osx${XR_PLATFORM_BITS}")
else()
  set(OPENVR_ARCH_TARGET "linux${XR_PLATFORM_BITS}")
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

find_package(Threads REQUIRED)

if(UNIX AND NOT APPLE)
  find_package(OpenGL REQUIRED)
  find_package(X11 REQUIRED)
endif()

find_package(nlohmann_json CONFIG QUIET)
if(TARGET nlohmann_json::nlohmann_json)
  set(XR_JSON_TARGET nlohmann_json::nlohmann_json)
else()
  find_path(NLOHMANN_JSON_INCLUDE_DIR NAMES nlohmann/json.hpp)
  if(NOT NLOHMANN_JSON_INCLUDE_DIR)
    message(FATAL_ERROR
      "nlohmann/json.hpp not found. Install nlohmann-json3-dev or pass -DNLOHMANN_JSON_INCLUDE_DIR=/path/to/include"
    )
  endif()
  add_library(xr_nlohmann_json INTERFACE)
  target_include_directories(xr_nlohmann_json INTERFACE "${NLOHMANN_JSON_INCLUDE_DIR}")
  set(XR_JSON_TARGET xr_nlohmann_json)
endif()

add_executable(steamvr_video_overlay
  src/main.cpp
)

target_include_directories(steamvr_video_overlay PRIVATE
  "${OPENVR_INCLUDE_DIR}"
  "${XR_SHARED_INCLUDE_DIR}"
)

if(UNIX AND NOT APPLE)
  target_include_directories(steamvr_video_overlay PRIVATE
    ${OPENGL_INCLUDE_DIR}
    ${X11_INCLUDE_DIR}
  )
endif()

target_link_libraries(steamvr_video_overlay PRIVATE
  Threads::Threads
  ${XR_JSON_TARGET}
  "${OPENVR_LIBRARIES}"
)

if(UNIX AND NOT APPLE)
  target_link_libraries(steamvr_video_overlay PRIVATE rt dl ${OPENGL_LIBRARIES} ${X11_LIBRARIES})
  set_target_properties(steamvr_video_overlay PROPERTIES
    BUILD_RPATH "$ORIGIN"
    INSTALL_RPATH "$ORIGIN"
  )
endif()

target_compile_options(steamvr_video_overlay PRIVATE
  -Wall
  -Wextra
  -Wpedantic
)

install(TARGETS steamvr_video_overlay
  RUNTIME DESTINATION bin/apps/steamvr/video_overlay
)

message(STATUS "OpenVR SDK: ${XR_OPENVR_SDK_ROOT}")
message(STATUS "OpenVR architecture target: ${OPENVR_ARCH_TARGET}")
message(STATUS "OpenVR library: ${OPENVR_LIBRARIES}")
message(STATUS "XR tracking root: ${XR_TRACKING_ROOT}")
