cmake_minimum_required(VERSION 3.20)

project(xr_mercury_hand_tracking_backend LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(XR_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." CACHE PATH "Path to xr_tracking repository root")

find_package(Threads REQUIRED)
set(CLI11_INCLUDE_DIR "" CACHE PATH "Optional CLI11 include directory")
set(NLOHMANN_JSON_INCLUDE_DIR "" CACHE PATH "Optional nlohmann_json include directory")
find_package(nlohmann_json CONFIG QUIET)
find_package(CLI11 CONFIG QUIET)

add_executable(capture_hand_tracking_backend
  "${CMAKE_CURRENT_LIST_DIR}/src/capture_hand_tracking_backend.cpp"
)

target_include_directories(capture_hand_tracking_backend PRIVATE
  "${XR_ROOT}/shared/include"
  "${CMAKE_CURRENT_LIST_DIR}/include"
)

target_link_libraries(capture_hand_tracking_backend PRIVATE
  Threads::Threads
  ${CMAKE_DL_LIBS}
)

foreach(dependency IN ITEMS CLI11 nlohmann_json)
  if(TARGET ${dependency}::${dependency})
    target_link_libraries(capture_hand_tracking_backend PRIVATE ${dependency}::${dependency})
  else()
    if(dependency STREQUAL "CLI11")
      set(dependency_include "${CLI11_INCLUDE_DIR}")
    else()
      set(dependency_include "${NLOHMANN_JSON_INCLUDE_DIR}")
    endif()
    if(NOT dependency_include)
      message(FATAL_ERROR "${dependency} not found; provide its package or include directory.")
    endif()
    target_include_directories(capture_hand_tracking_backend PRIVATE "${dependency_include}")
  endif()
endforeach()

if(UNIX AND NOT APPLE)
  target_link_libraries(capture_hand_tracking_backend PRIVATE rt)
endif()

target_compile_options(capture_hand_tracking_backend PRIVATE
  -Wall
  -Wextra
  -Wno-unused-parameter
)
