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
  src/core/backend_control.cpp
  src/core/config_io.cpp
  src/core/controller_input_publisher.cpp
  src/imu/controller_imu_processor.cpp
  src/imu/controller_imu_transform.cpp
  src/providers/gearvr/gearvr_input_provider.cpp
  src/providers/gearvr/gearvr_imu_stream.cpp
  src/providers/gearvr/gearvr_config_migration.cpp
  src/providers/gearvr/gearvr_options.cpp
  src/providers/gearvr/gearvr_protocol.cpp
  src/providers/gearvr/gearvr_touchpad.cpp
  src/providers/xiao_nrf54l15/xiao_nrf54l15_input_provider.cpp
  src/providers/xiao_nrf54l15/xiao_nrf54l15_options.cpp
  src/providers/xiao_nrf54l15/xr_controller_v1_protocol.cpp
  src/platform/input_provider.cpp
  src/platform/composite_input_provider.cpp
)

if(WIN32)
  target_sources(override_controller PRIVATE
    src/platform/windows/windows_input_provider.cpp
    src/providers/gearvr/transport/windows_gearvr_ble_transport.cpp
    src/providers/xiao_nrf54l15/transport/unsupported_xiao_nrf54l15_serial_transport.cpp
  )
elseif(UNIX AND NOT APPLE)
  target_sources(override_controller PRIVATE
    src/platform/linux/linux_evdev_input_provider.cpp
    src/providers/gearvr/transport/linux_gearvr_ble_transport.cpp
    src/providers/xiao_nrf54l15/transport/linux_xiao_nrf54l15_serial_transport.cpp
  )
else()
  target_sources(override_controller PRIVATE
    src/providers/gearvr/transport/unsupported_gearvr_ble_transport.cpp
    src/providers/xiao_nrf54l15/transport/unsupported_xiao_nrf54l15_serial_transport.cpp
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
  target_link_libraries(override_controller PRIVATE rt dl)
endif()

install(TARGETS override_controller
  RUNTIME DESTINATION bin/override_controller
)

set(OVERRIDE_CONTROLLER_CONFIG_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/configs")
if(EXISTS "${OVERRIDE_CONTROLLER_CONFIG_SOURCE_DIR}")
  add_custom_command(TARGET override_controller POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:override_controller>/configs"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
      "${OVERRIDE_CONTROLLER_CONFIG_SOURCE_DIR}"
      "$<TARGET_FILE_DIR:override_controller>/configs"
    COMMENT "Copy override_controller bundled configs next to binary"
  )

  install(DIRECTORY "${OVERRIDE_CONTROLLER_CONFIG_SOURCE_DIR}/"
    DESTINATION bin/override_controller/configs
    FILES_MATCHING PATTERN "*.json"
  )
endif()

set(OVERRIDE_CONTROLLER_TOOL_FILES
  "${CMAKE_CURRENT_LIST_DIR}/debug/calibrate_controller_accelerometer.py"
  "${CMAKE_CURRENT_LIST_DIR}/debug/record_controller_imu.py"
  "${CMAKE_CURRENT_LIST_DIR}/debug/calibrate_imu_axes.py"
  "${CMAKE_CURRENT_LIST_DIR}/debug/calibrate_controller_orientation_offset.py"
  "${CMAKE_CURRENT_LIST_DIR}/debug/calibrate_optical_grip.py"
  "${CMAKE_CURRENT_LIST_DIR}/debug/apply_optical_grip_calibration.py"
  "${CMAKE_CURRENT_LIST_DIR}/debug/calibrate_grip.sh"
  "${CMAKE_CURRENT_LIST_DIR}/debug/watch_controller_input_stream.py"
)
add_custom_command(TARGET override_controller POST_BUILD
  COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:override_controller>/tools"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different
    ${OVERRIDE_CONTROLLER_TOOL_FILES}
    "$<TARGET_FILE_DIR:override_controller>/tools"
  COMMENT "Copy override_controller Python tools next to binary"
)
install(PROGRAMS ${OVERRIDE_CONTROLLER_TOOL_FILES}
  DESTINATION bin/override_controller/tools
)
add_custom_command(TARGET override_controller POST_BUILD
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different
    "${CMAKE_CURRENT_LIST_DIR}/debug/calibration_requirements.txt"
    "${CMAKE_CURRENT_LIST_DIR}/docs/imu_optical_grip_calibration.md"
    "$<TARGET_FILE_DIR:override_controller>/tools"
)
install(FILES
  "${CMAKE_CURRENT_LIST_DIR}/debug/calibration_requirements.txt"
  "${CMAKE_CURRENT_LIST_DIR}/docs/imu_optical_grip_calibration.md"
  DESTINATION bin/override_controller/tools
)

option(OVERRIDE_CONTROLLER_BUILD_TESTS "Build override_controller protocol tests" OFF)
if(OVERRIDE_CONTROLLER_BUILD_TESTS)
  enable_testing()
  add_executable(serial_imu_clock_test tests/serial_imu_clock_test.cpp)
  target_include_directories(serial_imu_clock_test PRIVATE "${XR_SHARED_INCLUDE_DIR}")
  add_test(NAME serial_imu_clock_test COMMAND serial_imu_clock_test)
  add_executable(controller_imu_gravity_test
    tests/controller_imu_gravity_test.cpp
    src/imu/controller_imu_processor.cpp
    src/core/backend_control.cpp
  )
  target_include_directories(controller_imu_gravity_test PRIVATE "${CMAKE_CURRENT_LIST_DIR}/include")
  target_link_libraries(controller_imu_gravity_test PRIVATE Threads::Threads)
  add_test(NAME controller_imu_gravity_test COMMAND controller_imu_gravity_test)
  add_executable(controller_imu_calibration_test
    tests/controller_imu_calibration_test.cpp
    src/imu/controller_imu_processor.cpp
    src/core/backend_control.cpp
    src/core/config_io.cpp
    src/core/types.cpp
  )
  target_include_directories(controller_imu_calibration_test PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/include" "${XR_SHARED_INCLUDE_DIR}")
  if(TARGET nlohmann_json::nlohmann_json)
    target_link_libraries(controller_imu_calibration_test PRIVATE nlohmann_json::nlohmann_json)
  else()
    target_include_directories(controller_imu_calibration_test PRIVATE "${NLOHMANN_JSON_INCLUDE_DIR}")
  endif()
  target_link_libraries(controller_imu_calibration_test PRIVATE Threads::Threads)
  add_test(NAME controller_imu_calibration_test COMMAND controller_imu_calibration_test)
  find_package(Python3 COMPONENTS Interpreter QUIET)
  if(Python3_Interpreter_FOUND)
    add_test(NAME calibrate_controller_accelerometer_test
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/tests/calibrate_controller_accelerometer_test.py")
    if(UNIX AND NOT APPLE)
      execute_process(COMMAND "${Python3_EXECUTABLE}" -c "import numpy; import scipy"
        RESULT_VARIABLE _OPTICAL_CALIBRATION_DEPS_AVAILABLE
        OUTPUT_QUIET ERROR_QUIET)
      if(_OPTICAL_CALIBRATION_DEPS_AVAILABLE EQUAL 0)
        add_test(NAME calibrate_optical_grip_test
          COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/tests/calibrate_optical_grip_test.py")
      else()
        message(STATUS "Optical grip calibration tests skipped: install NumPy/SciPy in the configured Python")
      endif()
    endif()
  endif()
  add_executable(gearvr_imu_stream_test
    tests/gearvr_imu_stream_test.cpp
    src/providers/gearvr/gearvr_imu_stream.cpp
    src/providers/gearvr/gearvr_protocol.cpp
    src/imu/controller_imu_processor.cpp
    src/core/backend_control.cpp
  )
  target_include_directories(gearvr_imu_stream_test PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/include"
    "${CMAKE_CURRENT_LIST_DIR}/src/providers/gearvr"
    "${XR_SHARED_INCLUDE_DIR}"
  )
  target_link_libraries(gearvr_imu_stream_test PRIVATE Threads::Threads)
  add_test(NAME gearvr_imu_stream_test COMMAND gearvr_imu_stream_test)
  add_executable(gearvr_input_provider_test
    tests/gearvr_input_provider_test.cpp
    src/providers/gearvr/gearvr_input_provider.cpp
    src/providers/gearvr/gearvr_imu_stream.cpp
    src/providers/gearvr/gearvr_protocol.cpp
    src/providers/gearvr/gearvr_options.cpp
    src/providers/gearvr/gearvr_config_migration.cpp
    src/providers/gearvr/gearvr_touchpad.cpp
    src/platform/composite_input_provider.cpp
    src/imu/controller_imu_processor.cpp
    src/core/backend_control.cpp
    src/core/types.cpp
  )
  target_include_directories(gearvr_input_provider_test PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/include"
    "${CMAKE_CURRENT_LIST_DIR}/src/providers/gearvr"
    "${CMAKE_CURRENT_LIST_DIR}/src/platform"
    "${XR_SHARED_INCLUDE_DIR}"
  )
  target_link_libraries(gearvr_input_provider_test PRIVATE Threads::Threads)
  if(TARGET nlohmann_json::nlohmann_json)
    target_link_libraries(gearvr_input_provider_test PRIVATE nlohmann_json::nlohmann_json)
  else()
    target_include_directories(gearvr_input_provider_test PRIVATE "${NLOHMANN_JSON_INCLUDE_DIR}")
  endif()
  add_test(NAME gearvr_input_provider_test COMMAND gearvr_input_provider_test)
  add_executable(xr_controller_v1_protocol_test
    tests/xr_controller_v1_protocol_test.cpp
    src/providers/xiao_nrf54l15/xr_controller_v1_protocol.cpp
  )
  target_include_directories(xr_controller_v1_protocol_test PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/src/providers/xiao_nrf54l15"
  )
  add_test(NAME xr_controller_v1_protocol_test COMMAND xr_controller_v1_protocol_test)

  add_executable(controller_imu_transform_test
    tests/controller_imu_transform_test.cpp
    src/imu/controller_imu_transform.cpp
  )
  target_include_directories(controller_imu_transform_test PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/include"
    "${XR_SHARED_INCLUDE_DIR}"
  )
  add_test(NAME controller_imu_transform_test COMMAND controller_imu_transform_test)

  if(UNIX AND NOT APPLE)
    add_executable(gearvr_bluez_retry_test
      tests/gearvr_bluez_retry_test.cpp
      src/providers/gearvr/gearvr_protocol.cpp
      src/core/backend_control.cpp
    )
    target_include_directories(gearvr_bluez_retry_test PRIVATE
      "${CMAKE_CURRENT_LIST_DIR}/include"
      "${XR_SHARED_INCLUDE_DIR}"
    )
    target_link_libraries(gearvr_bluez_retry_test PRIVATE Threads::Threads dl)
    target_link_options(gearvr_bluez_retry_test PRIVATE "-Wl,--wrap=dlsym")
    add_test(NAME gearvr_bluez_retry_test COMMAND gearvr_bluez_retry_test)

    add_executable(evdev_rescan_test
      tests/evdev_rescan_test.cpp
      src/platform/linux/linux_evdev_input_provider.cpp
      src/core/types.cpp
    )
    target_include_directories(evdev_rescan_test PRIVATE
      "${CMAKE_CURRENT_LIST_DIR}/include"
      "${CMAKE_CURRENT_LIST_DIR}/src/platform/linux"
      "${XR_SHARED_INCLUDE_DIR}"
    )
    target_link_libraries(evdev_rescan_test PRIVATE Threads::Threads)
    target_link_options(evdev_rescan_test PRIVATE "-Wl,--wrap=ioctl")
    add_test(NAME evdev_rescan_test COMMAND evdev_rescan_test)
  endif()
endif()
