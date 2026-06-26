cmake_minimum_required(VERSION 3.20)
project(capture_service_cpp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(OpenCV REQUIRED COMPONENTS core imgproc videoio)
find_package(Threads REQUIRED)

set(XR_CAPTURE_SERVICE_CPP_DEVICE "xreal_ultra" CACHE STRING "capture_service_cpp device/vendor target")
set_property(CACHE XR_CAPTURE_SERVICE_CPP_DEVICE PROPERTY STRINGS xreal_ultra)
if(NOT XR_CAPTURE_SERVICE_CPP_DEVICE STREQUAL "xreal_ultra")
  message(FATAL_ERROR "Unsupported XR_CAPTURE_SERVICE_CPP_DEVICE=${XR_CAPTURE_SERVICE_CPP_DEVICE}; supported: xreal_ultra")
endif()
message(STATUS "capture_service_cpp device target: ${XR_CAPTURE_SERVICE_CPP_DEVICE}")

find_path(HIDAPI_INCLUDE_DIR hidapi/hidapi.h HINTS ${HIDAPI_ROOT}/include $ENV{HIDAPI_ROOT}/include)
find_library(HIDAPI_LIBRARY NAMES hidapi-hidraw hidapi-libusb hidapi HINTS ${HIDAPI_ROOT}/lib $ENV{HIDAPI_ROOT}/lib)
if(NOT HIDAPI_INCLUDE_DIR OR NOT HIDAPI_LIBRARY)
  message(FATAL_ERROR "hidapi not found. Set HIDAPI_ROOT or install libhidapi-dev / hidapi package.")
endif()

set(CAPTURE_SERVICE_CPP_SOURCES
  src/common.cpp
  src/main.cpp
  src/stream_publishers.cpp
  src/tcp_fanout.cpp
  src/camera_pipeline.cpp
  src/imu_pipeline.cpp
  src/platform/hidapi_input_device.cpp
  src/vendor/xreal_camera_decoder.cpp
  src/vendor/xreal_camera_transform.cpp
  src/vendor/xreal_imu_codec.cpp
  src/vendor/xreal_stream_specs.cpp
)

if(WIN32)
  list(APPEND CAPTURE_SERVICE_CPP_SOURCES
    src/platform/windows/camera_capture_windows.cpp
    src/platform/windows/capabilities_windows.cpp
    src/platform/windows/process_windows.cpp
    src/platform/windows/runtime_defaults_windows.cpp
    src/platform/windows/shm_publisher_windows.cpp
  )
else()
  list(APPEND CAPTURE_SERVICE_CPP_SOURCES
    src/platform/linux/camera_capture_linux.cpp
    src/platform/linux/capabilities_linux.cpp
    src/platform/linux/process_linux.cpp
    src/platform/linux/runtime_defaults_linux.cpp
    src/platform/linux/shm_publisher_linux.cpp
  )
endif()

add_executable(capture_service_cpp ${CAPTURE_SERVICE_CPP_SOURCES})
target_include_directories(capture_service_cpp PRIVATE include ${HIDAPI_INCLUDE_DIR})
target_link_libraries(capture_service_cpp PRIVATE ${OpenCV_LIBS} ${HIDAPI_LIBRARY} Threads::Threads)
if(WIN32)
  target_link_libraries(capture_service_cpp PRIVATE ws2_32)
endif()

install(TARGETS capture_service_cpp RUNTIME DESTINATION .)
