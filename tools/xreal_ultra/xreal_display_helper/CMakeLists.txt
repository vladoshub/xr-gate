cmake_minimum_required(VERSION 3.16)
project(xreal_display_helper LANGUAGES CXX)

add_executable(xreal_display_helper
  src/main.cpp
  src/common.cpp
)
target_compile_features(xreal_display_helper PRIVATE cxx_std_17)
target_include_directories(xreal_display_helper PRIVATE include)

if(WIN32)
  target_sources(xreal_display_helper PRIVATE
    src/platform/windows/hid_platform_windows.cpp
  )
  target_compile_definitions(xreal_display_helper PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)

  find_package(hidapi CONFIG QUIET)
  if(TARGET hidapi::hidapi)
    target_link_libraries(xreal_display_helper PRIVATE hidapi::hidapi)
  elseif(TARGET hidapi::hidapi-hidraw)
    target_link_libraries(xreal_display_helper PRIVATE hidapi::hidapi-hidraw)
  elseif(TARGET hidapi::hidapi-libusb)
    target_link_libraries(xreal_display_helper PRIVATE hidapi::hidapi-libusb)
  else()
    find_path(HIDAPI_INCLUDE_DIR NAMES hidapi/hidapi.h)
    find_library(HIDAPI_LIBRARY NAMES hidapi hidapi-hidraw hidapi-libusb)
    if(NOT HIDAPI_INCLUDE_DIR OR NOT HIDAPI_LIBRARY)
      message(FATAL_ERROR "hidapi not found. Install hidapi via vcpkg/conan or set HIDAPI_INCLUDE_DIR/HIDAPI_LIBRARY.")
    endif()
    target_include_directories(xreal_display_helper PRIVATE "${HIDAPI_INCLUDE_DIR}")
    target_link_libraries(xreal_display_helper PRIVATE "${HIDAPI_LIBRARY}")
  endif()
else()
  target_sources(xreal_display_helper PRIVATE
    src/platform/linux/hid_platform_linux.cpp
  )

  find_package(PkgConfig REQUIRED)

  pkg_check_modules(HIDAPI_HIDRAW QUIET IMPORTED_TARGET hidapi-hidraw)
  if(HIDAPI_HIDRAW_FOUND)
    target_link_libraries(xreal_display_helper PRIVATE PkgConfig::HIDAPI_HIDRAW)
  else()
    pkg_check_modules(HIDAPI_LIBUSB REQUIRED IMPORTED_TARGET hidapi-libusb)
    target_link_libraries(xreal_display_helper PRIVATE PkgConfig::HIDAPI_LIBUSB)
  endif()
endif()

install(TARGETS xreal_display_helper RUNTIME DESTINATION bin/xreal_display_helper)
