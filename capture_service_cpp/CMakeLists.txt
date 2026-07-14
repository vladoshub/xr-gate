cmake_minimum_required(VERSION 3.20)
project(capture_service_cpp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(OpenCV REQUIRED COMPONENTS core imgproc videoio)
find_package(Threads REQUIRED)

# All built-in source drivers are compiled into one hardware-independent
# service. Runtime selection is made by config.yaml, not by a device-specific
# build target.
message(STATUS "capture_service_cpp runtime-selectable camera/IMU sources enabled")

find_path(HIDAPI_INCLUDE_DIR hidapi/hidapi.h HINTS ${HIDAPI_ROOT}/include $ENV{HIDAPI_ROOT}/include)
find_library(HIDAPI_LIBRARY NAMES hidapi-hidraw hidapi-libusb hidapi HINTS ${HIDAPI_ROOT}/lib $ENV{HIDAPI_ROOT}/lib)
if(NOT HIDAPI_INCLUDE_DIR OR NOT HIDAPI_LIBRARY)
  message(FATAL_ERROR "hidapi not found. Set HIDAPI_ROOT or install libhidapi-dev / hidapi package.")
endif()

set(CAPTURE_SERVICE_CPP_SOURCES
  src/common.cpp
  src/main.cpp
  src/image_transform.cpp
  src/stream_publishers.cpp
  src/tcp_fanout.cpp
  src/camera_pipeline.cpp
  src/imu_pipeline.cpp
  src/config/capture_config.cpp
  src/platform/hidapi_input_device.cpp
  src/sources/camera_source_factory.cpp
  src/sources/xreal_camera_source.cpp
  src/sources/opencv_stereo_camera_source.cpp
  src/sources/imu_source_factory.cpp
  src/sources/xreal_hid_imu_source.cpp
  src/sources/serial_imu_source.cpp
  src/sources/serial_imu_protocol.cpp
  src/protocols/xr_imu_v1.cpp
  src/timing/affine_device_clock_mapper.cpp
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
    src/platform/windows/serial_port_windows.cpp
    src/platform/windows/shm_publisher_windows.cpp
  )
else()
  list(APPEND CAPTURE_SERVICE_CPP_SOURCES
    src/platform/linux/camera_capture_linux.cpp
    src/platform/linux/capabilities_linux.cpp
    src/platform/linux/process_linux.cpp
    src/platform/linux/runtime_defaults_linux.cpp
    src/platform/linux/serial_port_linux.cpp
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
install(DIRECTORY configs/ DESTINATION configs OPTIONAL)

include(CTest)
if(BUILD_TESTING)
  if(WIN32)
    set(CAPTURE_CONFIG_TEST_PLATFORM_SOURCE src/platform/windows/runtime_defaults_windows.cpp)
  else()
    set(CAPTURE_CONFIG_TEST_PLATFORM_SOURCE src/platform/linux/runtime_defaults_linux.cpp)
  endif()

  add_executable(capture_config_legacy_smoke
    tests/capture_config_legacy_smoke.cpp
    src/common.cpp
    src/config/capture_config.cpp
    ${CAPTURE_CONFIG_TEST_PLATFORM_SOURCE}
  )
  target_include_directories(capture_config_legacy_smoke PRIVATE include)
  add_test(NAME capture_config_legacy_smoke COMMAND capture_config_legacy_smoke)

  add_executable(xr_imu_v1_smoke
    tests/xr_imu_v1_smoke.cpp
    src/protocols/xr_imu_v1.cpp
  )
  target_include_directories(xr_imu_v1_smoke PRIVATE include)
  add_test(NAME xr_imu_v1_smoke COMMAND xr_imu_v1_smoke)

  add_executable(affine_device_clock_mapper_smoke
    tests/affine_device_clock_mapper_smoke.cpp
    src/timing/affine_device_clock_mapper.cpp
  )
  target_include_directories(affine_device_clock_mapper_smoke PRIVATE include)
  add_test(NAME affine_device_clock_mapper_smoke COMMAND affine_device_clock_mapper_smoke)

  if(NOT WIN32)
    add_executable(serial_imu_transport_activity_linux
      tests/serial_imu_transport_activity_linux.cpp
      src/sources/serial_imu_source.cpp
      src/platform/linux/serial_port_linux.cpp
      src/protocols/xr_imu_v1.cpp
      src/timing/affine_device_clock_mapper.cpp
    )
    target_include_directories(serial_imu_transport_activity_linux PRIVATE include)
    add_test(NAME serial_imu_transport_activity_linux COMMAND serial_imu_transport_activity_linux)
  endif()
endif()
