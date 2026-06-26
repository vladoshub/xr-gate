cmake_minimum_required(VERSION 3.20)

project(xr_mercury_hand_tracking_backend LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(XR_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." CACHE PATH "Path to xr_tracking repository root")

find_package(Threads REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(CLI11 REQUIRED)

add_executable(capture_hand_tracking_backend
  "${CMAKE_CURRENT_LIST_DIR}/src/capture_hand_tracking_backend.cpp"
)

target_include_directories(capture_hand_tracking_backend PRIVATE
  "${XR_ROOT}/shared/include"
  "${CMAKE_CURRENT_LIST_DIR}/include"
)

target_link_libraries(capture_hand_tracking_backend PRIVATE
  Threads::Threads
  nlohmann_json::nlohmann_json
  CLI11::CLI11
  ${CMAKE_DL_LIBS}
)

if(UNIX AND NOT APPLE)
  target_link_libraries(capture_hand_tracking_backend PRIVATE rt)
endif()

target_compile_options(capture_hand_tracking_backend PRIVATE
  -Wall
  -Wextra
  -Wno-unused-parameter
)
