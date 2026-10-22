cmake_minimum_required(VERSION 3.16)
project(xr_runtime_adapter LANGUAGES CXX)

set(XR_TRACKING_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." CACHE PATH "XR tracking project root")
set(XR_SHARED_INCLUDE_DIR "${XR_TRACKING_ROOT}/shared/include" CACHE PATH "Shared XR tracking include directory")
set(CLI11_INCLUDE_DIR "" CACHE PATH "Optional CLI11 include directory")
set(NLOHMANN_JSON_INCLUDE_DIR "" CACHE PATH "Optional nlohmann_json include directory")

if(NOT EXISTS "${XR_SHARED_INCLUDE_DIR}")
  message(FATAL_ERROR "XR_SHARED_INCLUDE_DIR not found: ${XR_SHARED_INCLUDE_DIR}")
endif()

find_package(Threads REQUIRED)
find_package(CLI11 CONFIG QUIET)
find_package(nlohmann_json CONFIG QUIET)

add_executable(xr_runtime_adapter
  src/xr_runtime_adapter.cpp
  src/app_tcp_server.cpp
  src/coordinate_util.cpp
  src/gestures.cpp
  src/hmd_pose_stability_filter.cpp
  src/jitter_filter.cpp
  src/body_tracker_stability_filter.cpp
  src/override_controller.cpp
)

target_compile_features(xr_runtime_adapter PRIVATE cxx_std_17)
target_include_directories(xr_runtime_adapter PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/src"
  "${XR_SHARED_INCLUDE_DIR}"
)
target_link_libraries(xr_runtime_adapter PRIVATE Threads::Threads)
if(WIN32)
  target_link_libraries(xr_runtime_adapter PRIVATE ws2_32)
endif()

if(TARGET CLI11::CLI11)
  target_link_libraries(xr_runtime_adapter PRIVATE CLI11::CLI11)
elseif(CLI11_INCLUDE_DIR)
  target_include_directories(xr_runtime_adapter PRIVATE "${CLI11_INCLUDE_DIR}")
else()
  message(FATAL_ERROR "CLI11 not found. Provide CLI11 via CMAKE_PREFIX_PATH or set CLI11_INCLUDE_DIR.")
endif()

if(TARGET nlohmann_json::nlohmann_json)
  target_link_libraries(xr_runtime_adapter PRIVATE nlohmann_json::nlohmann_json)
elseif(NLOHMANN_JSON_INCLUDE_DIR)
  target_include_directories(xr_runtime_adapter PRIVATE "${NLOHMANN_JSON_INCLUDE_DIR}")
else()
  message(FATAL_ERROR "nlohmann_json not found. Provide nlohmann_json via CMAKE_PREFIX_PATH or set NLOHMANN_JSON_INCLUDE_DIR.")
endif()

if(MSVC)
  target_compile_options(xr_runtime_adapter PRIVATE /W4 /permissive-)
  target_compile_definitions(xr_runtime_adapter PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
else()
  target_compile_options(xr_runtime_adapter PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

install(TARGETS xr_runtime_adapter RUNTIME DESTINATION bin/runtime_adapters/xr_runtime_adapter)

option(XR_RUNTIME_ADAPTER_BUILD_TESTS "Build runtime tracking regression tests" OFF)
if(XR_RUNTIME_ADAPTER_BUILD_TESTS AND UNIX AND NOT APPLE)
  enable_testing()
  add_executable(hmd_pose_stability_test tests/hmd_pose_stability_test.cpp src/hmd_pose_stability_filter.cpp)
  target_compile_features(hmd_pose_stability_test PRIVATE cxx_std_17)
  target_include_directories(hmd_pose_stability_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src" "${XR_SHARED_INCLUDE_DIR}")
  if(TARGET nlohmann_json::nlohmann_json)
    target_link_libraries(hmd_pose_stability_test PRIVATE nlohmann_json::nlohmann_json)
  else()
    target_include_directories(hmd_pose_stability_test PRIVATE "${NLOHMANN_JSON_INCLUDE_DIR}")
  endif()
  add_test(NAME hmd_pose_stability_test COMMAND hmd_pose_stability_test)
  find_package(Python3 COMPONENTS Interpreter QUIET)
  if(Python3_Interpreter_FOUND)
    add_test(NAME app_tcp_server_test
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/app_tcp_server_test.py"
        --adapter $<TARGET_FILE:xr_runtime_adapter>)
    set_tests_properties(app_tcp_server_test PROPERTIES TIMEOUT 30)
    add_test(NAME controller_yaw_configuration_test
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/controller_yaw_configuration_test.py"
        --adapter $<TARGET_FILE:xr_runtime_adapter>)
    set_tests_properties(controller_yaw_configuration_test PROPERTIES TIMEOUT 15)
    add_test(NAME hmd_age_configuration_test
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/hmd_age_configuration_test.py"
        --adapter $<TARGET_FILE:xr_runtime_adapter>)
    set_tests_properties(hmd_age_configuration_test PROPERTIES TIMEOUT 15)
  endif()
  add_executable(controller_imu_angular_velocity_test
    tests/controller_imu_angular_velocity_test.cpp
    src/override_controller.cpp
  )
  target_compile_features(controller_imu_angular_velocity_test PRIVATE cxx_std_17)
  target_include_directories(controller_imu_angular_velocity_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${XR_SHARED_INCLUDE_DIR}"
  )
  if(TARGET nlohmann_json::nlohmann_json)
    target_link_libraries(controller_imu_angular_velocity_test PRIVATE nlohmann_json::nlohmann_json)
  else()
    target_include_directories(controller_imu_angular_velocity_test PRIVATE "${NLOHMANN_JSON_INCLUDE_DIR}")
  endif()
  add_test(NAME controller_imu_angular_velocity_test COMMAND controller_imu_angular_velocity_test)

  add_executable(controller_imu_delivery_test
    tests/controller_imu_delivery_test.cpp
    src/override_controller.cpp
    "${XR_TRACKING_ROOT}/override_controller/src/core/controller_input_publisher.cpp"
    "${XR_TRACKING_ROOT}/override_controller/src/core/types.cpp"
  )
  target_compile_features(controller_imu_delivery_test PRIVATE cxx_std_17)
  target_include_directories(controller_imu_delivery_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${XR_TRACKING_ROOT}/override_controller/include"
    "${XR_SHARED_INCLUDE_DIR}"
  )
  if(TARGET nlohmann_json::nlohmann_json)
    target_link_libraries(controller_imu_delivery_test PRIVATE nlohmann_json::nlohmann_json)
  else()
    target_include_directories(controller_imu_delivery_test PRIVATE "${NLOHMANN_JSON_INCLUDE_DIR}")
  endif()
  target_link_libraries(controller_imu_delivery_test PRIVATE Threads::Threads rt)
  add_test(NAME controller_imu_delivery_test COMMAND controller_imu_delivery_test)

  add_executable(xr_controller_imu_delivery_test
    tests/xr_controller_imu_delivery_test.cpp
    src/override_controller.cpp
    "${XR_TRACKING_ROOT}/override_controller/src/providers/xiao_nrf54l15/xiao_nrf54l15_input_provider.cpp"
    "${XR_TRACKING_ROOT}/override_controller/src/providers/xiao_nrf54l15/xiao_nrf54l15_options.cpp"
    "${XR_TRACKING_ROOT}/override_controller/src/providers/xiao_nrf54l15/xr_controller_v1_protocol.cpp"
    "${XR_TRACKING_ROOT}/override_controller/src/imu/controller_imu_processor.cpp"
    "${XR_TRACKING_ROOT}/override_controller/src/core/controller_input_publisher.cpp"
    "${XR_TRACKING_ROOT}/override_controller/src/core/backend_control.cpp"
    "${XR_TRACKING_ROOT}/override_controller/src/core/types.cpp"
    "${XR_TRACKING_ROOT}/capture_service_cpp/src/protocols/xr_controller_v1.cpp"
  )
  target_compile_features(xr_controller_imu_delivery_test PRIVATE cxx_std_17)
  target_include_directories(xr_controller_imu_delivery_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${XR_TRACKING_ROOT}/override_controller/include"
    "${XR_TRACKING_ROOT}/override_controller/src/providers/xiao_nrf54l15"
    "${XR_TRACKING_ROOT}/capture_service_cpp/include"
    "${XR_SHARED_INCLUDE_DIR}"
  )
  if(TARGET nlohmann_json::nlohmann_json)
    target_link_libraries(xr_controller_imu_delivery_test PRIVATE nlohmann_json::nlohmann_json)
  else()
    target_include_directories(xr_controller_imu_delivery_test PRIVATE "${NLOHMANN_JSON_INCLUDE_DIR}")
  endif()
  target_link_libraries(xr_controller_imu_delivery_test PRIVATE Threads::Threads rt)
  add_test(NAME xr_controller_imu_delivery_test COMMAND xr_controller_imu_delivery_test)

  add_executable(controller_imu_prediction_test
    tests/controller_imu_prediction_test.cpp
    src/override_controller.cpp
    src/coordinate_util.cpp
    "${XR_TRACKING_ROOT}/override_controller/src/imu/controller_imu_processor.cpp"
    "${XR_TRACKING_ROOT}/override_controller/src/core/backend_control.cpp"
  )
  target_compile_features(controller_imu_prediction_test PRIVATE cxx_std_17)
  target_include_directories(controller_imu_prediction_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${XR_TRACKING_ROOT}/override_controller/include"
    "${XR_SHARED_INCLUDE_DIR}"
  )
  if(TARGET nlohmann_json::nlohmann_json)
    target_link_libraries(controller_imu_prediction_test PRIVATE nlohmann_json::nlohmann_json)
  else()
    target_include_directories(controller_imu_prediction_test PRIVATE "${NLOHMANN_JSON_INCLUDE_DIR}")
  endif()
  target_link_libraries(controller_imu_prediction_test PRIVATE Threads::Threads)
  add_test(NAME controller_imu_prediction_test COMMAND controller_imu_prediction_test)

  add_library(mercury_velocity_mock SHARED tests/mercury_velocity_mock.cpp)
  add_executable(hand_optical_velocity_test
    tests/hand_optical_velocity_test.cpp
    src/override_controller.cpp
  )
  foreach(velocity_target mercury_velocity_mock hand_optical_velocity_test)
    target_compile_features(${velocity_target} PRIVATE cxx_std_17)
    target_include_directories(${velocity_target} PRIVATE
      "${CMAKE_CURRENT_SOURCE_DIR}/src"
      "${XR_TRACKING_ROOT}/backends/mercury_hand_tracking/include"
      "${XR_SHARED_INCLUDE_DIR}"
    )
    if(TARGET nlohmann_json::nlohmann_json)
      target_link_libraries(${velocity_target} PRIVATE nlohmann_json::nlohmann_json)
    else()
      target_include_directories(${velocity_target} PRIVATE "${NLOHMANN_JSON_INCLUDE_DIR}")
    endif()
    target_link_libraries(${velocity_target} PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
  endforeach()
  add_dependencies(hand_optical_velocity_test mercury_velocity_mock)
  add_test(NAME hand_optical_velocity_test
    COMMAND hand_optical_velocity_test $<TARGET_FILE:mercury_velocity_mock>)
endif()
