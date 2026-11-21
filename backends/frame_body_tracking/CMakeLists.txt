cmake_minimum_required(VERSION 3.18)
project(xr_frame_body_tracking LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

get_filename_component(XR_TRACKING_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(ORT_ROOT "" CACHE PATH "Path to the XR Gate ONNX Runtime SDK")
if(ORT_ROOT)
  set(ORT_INCLUDE_DIR "${ORT_ROOT}/include")
  set(ORT_LIBRARY "${ORT_ROOT}/lib/libonnxruntime.so")
  if(NOT EXISTS "${ORT_INCLUDE_DIR}/onnxruntime_cxx_api.h" OR NOT EXISTS "${ORT_LIBRARY}")
    message(FATAL_ERROR "Incomplete ONNX Runtime SDK at ${ORT_ROOT}")
  endif()
else()
  find_path(ORT_INCLUDE_DIR onnxruntime_cxx_api.h REQUIRED)
  find_library(ORT_LIBRARY onnxruntime REQUIRED)
endif()
find_path(CLI11_INCLUDE_DIR CLI/CLI.hpp REQUIRED)
find_path(NLOHMANN_JSON_INCLUDE_DIR nlohmann/json.hpp REQUIRED)
find_package(Eigen3 3.3 REQUIRED NO_MODULE)
find_package(Threads REQUIRED)

add_executable(frame_body_tracking_backend
  src/frame_body_backend.cpp
  src/frame_model.cpp
)
target_include_directories(frame_body_tracking_backend PRIVATE "${XR_TRACKING_ROOT}/shared/include")
target_include_directories(frame_body_tracking_backend SYSTEM PRIVATE
  "${ORT_INCLUDE_DIR}" "${CLI11_INCLUDE_DIR}" "${NLOHMANN_JSON_INCLUDE_DIR}"
)
target_link_libraries(frame_body_tracking_backend PRIVATE
  "${ORT_LIBRARY}" Eigen3::Eigen Threads::Threads
)
target_compile_options(frame_body_tracking_backend PRIVATE -Wall -Wextra -Wpedantic)

get_filename_component(ORT_PACKAGE_DIR "${ORT_ROOT}" NAME)
set_target_properties(frame_body_tracking_backend PROPERTIES
  INSTALL_RPATH "$ORIGIN/../../onnxruntime/${ORT_PACKAGE_DIR}/lib"
)
install(TARGETS frame_body_tracking_backend RUNTIME DESTINATION .)
