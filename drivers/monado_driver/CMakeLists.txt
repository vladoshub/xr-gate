# Intended to be included from inside a Monado checkout, after Monado has set up
# include paths for xrt/ and util/. This is not a standalone project.

add_library(drv_xr_tracking_runtime STATIC
  src/runtime_readers.cpp
  src/posix_runtime_readers.cpp
  src/windows_runtime_readers.cpp
  src/xr_monado_driver.cpp
)

if(NOT DEFINED XR_TRACKING_ROOT)
  if(DEFINED ENV{XR_TRACKING_ROOT})
    set(XR_TRACKING_ROOT "$ENV{XR_TRACKING_ROOT}")
  else()
    # Case A: project-owned source at drivers/monado_driver.
    get_filename_component(_xr_tracking_root_from_project "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
    # Case B: copied source inside third_party/monado_driver/src/xrt/drivers/xr_tracking_runtime.
    get_filename_component(_xr_tracking_root_from_monado_copy "${CMAKE_CURRENT_SOURCE_DIR}/../../../../../.." ABSOLUTE)

    if(EXISTS "${_xr_tracking_root_from_project}/shared/include/xr_runtime/contracts/runtime_pose_stream.hpp")
      set(XR_TRACKING_ROOT "${_xr_tracking_root_from_project}")
    elseif(EXISTS "${_xr_tracking_root_from_monado_copy}/shared/include/xr_runtime/contracts/runtime_pose_stream.hpp")
      set(XR_TRACKING_ROOT "${_xr_tracking_root_from_monado_copy}")
    endif()
  endif()
endif()

set(_xr_tracking_shared_include "${XR_TRACKING_ROOT}/shared/include")
set(_xr_tracking_runtime_pose_header "${_xr_tracking_shared_include}/xr_runtime/contracts/runtime_pose_stream.hpp")
set(_xr_tracking_controller_state_header "${_xr_tracking_shared_include}/xr_runtime/contracts/runtime_controller_state_contract.hpp")
set(_xr_tracking_legacy_hand_header "${_xr_tracking_shared_include}/xr_tracking/publishers/hand_tracking_shm_publisher.hpp")

if(NOT DEFINED XR_TRACKING_ROOT
   OR NOT EXISTS "${_xr_tracking_runtime_pose_header}"
   OR NOT EXISTS "${_xr_tracking_controller_state_header}"
   OR NOT EXISTS "${_xr_tracking_legacy_hand_header}")
  message(FATAL_ERROR
    "XR_TRACKING_ROOT is not set correctly; expected shared headers under ${XR_TRACKING_ROOT}/shared/include. "
    "Missing one of: ${_xr_tracking_runtime_pose_header}, ${_xr_tracking_controller_state_header}, ${_xr_tracking_legacy_hand_header}")
endif()

target_include_directories(drv_xr_tracking_runtime PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${_xr_tracking_shared_include}
)

target_compile_features(drv_xr_tracking_runtime PRIVATE cxx_std_17)
target_link_libraries(drv_xr_tracking_runtime PRIVATE xrt-interfaces aux_util)

if(UNIX AND NOT APPLE)
  target_link_libraries(drv_xr_tracking_runtime PRIVATE rt)
endif()
