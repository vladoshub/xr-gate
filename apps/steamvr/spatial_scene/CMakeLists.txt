cmake_minimum_required(VERSION 3.16)
project(xr_steamvr_spatial_scene LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

get_filename_component(XR_TRACKING_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
set(XR_SHARED_INCLUDE_DIR "${XR_TRACKING_ROOT}/shared/include" CACHE PATH "XR shared include directory")
set(XR_OPENVR_SDK_DIR "${XR_TRACKING_ROOT}/third_party/openvr" CACHE PATH "OpenVR SDK directory")

find_package(OpenGL REQUIRED)
find_package(X11 REQUIRED)
find_package(Threads REQUIRED)

find_path(OPENVR_INCLUDE_DIR
  NAMES openvr.h
  PATHS
    "${XR_OPENVR_SDK_DIR}/headers"
    "${XR_OPENVR_SDK_DIR}/src/headers"
    "${XR_OPENVR_SDK_DIR}/include"
  NO_DEFAULT_PATH)

find_library(OPENVR_API_LIBRARY
  NAMES openvr_api
  PATHS
    "${XR_OPENVR_SDK_DIR}/lib/linux64"
    "${XR_OPENVR_SDK_DIR}/bin/linux64"
    "${XR_OPENVR_SDK_DIR}/lib"
  NO_DEFAULT_PATH)

if(NOT OPENVR_INCLUDE_DIR OR NOT OPENVR_API_LIBRARY)
  message(FATAL_ERROR
    "OpenVR SDK not found. Expected openvr.h and libopenvr_api.so under ${XR_OPENVR_SDK_DIR}. "
    "Set XR_OPENVR_SDK_DIR=/path/to/openvr or install/fetch third_party/openvr first.")
endif()

message(STATUS "XR SteamVR spatial scene OpenVR include: ${OPENVR_INCLUDE_DIR}")
message(STATUS "XR SteamVR spatial scene OpenVR lib: ${OPENVR_API_LIBRARY}")
message(STATUS "XR SteamVR spatial scene shared include: ${XR_SHARED_INCLUDE_DIR}")

add_executable(xr_steamvr_spatial_scene
  src/xr_steamvr_spatial_scene.cpp)

target_include_directories(xr_steamvr_spatial_scene PRIVATE
  "${OPENVR_INCLUDE_DIR}"
  "${XR_SHARED_INCLUDE_DIR}"
  ${X11_INCLUDE_DIR}
  ${OPENGL_INCLUDE_DIR})

target_link_libraries(xr_steamvr_spatial_scene PRIVATE
  "${OPENVR_API_LIBRARY}"
  Threads::Threads
  dl)

if(TARGET OpenGL::GL)
  target_link_libraries(xr_steamvr_spatial_scene PRIVATE OpenGL::GL)
else()
  target_link_libraries(xr_steamvr_spatial_scene PRIVATE ${OPENGL_LIBRARIES})
endif()
if(TARGET X11::X11)
  target_link_libraries(xr_steamvr_spatial_scene PRIVATE X11::X11)
else()
  target_link_libraries(xr_steamvr_spatial_scene PRIVATE ${X11_LIBRARIES})
endif()

set_target_properties(xr_steamvr_spatial_scene PROPERTIES
  BUILD_WITH_INSTALL_RPATH ON
  INSTALL_RPATH "$ORIGIN/lib")

target_compile_options(xr_steamvr_spatial_scene PRIVATE -Wall -Wextra -Wpedantic)
