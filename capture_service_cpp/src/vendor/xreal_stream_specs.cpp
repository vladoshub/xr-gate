#include "capture_service_cpp/vendor/xreal_stream_specs.hpp"

namespace xr_capture_cpp {

StreamSpec make_xreal_camera0_stream(uint32_t slot_count) {
  return StreamSpec{"camera0", kKindImage, "IMAGE", 480, 640, kFormatGray8, "GRAY8", 480 * 640, slot_count, "camera0", "Experimental native C++ front-left camera frame"};
}

StreamSpec make_xreal_camera1_stream(uint32_t slot_count) {
  return StreamSpec{"camera1", kKindImage, "IMAGE", 480, 640, kFormatGray8, "GRAY8", 480 * 640, slot_count, "camera1", "Experimental native C++ front-right camera frame"};
}

StreamSpec make_xreal_raw_hid_stream(uint32_t slot_count) {
  return StreamSpec{"xreal_raw_hid", kKindBytes, "BYTES", 0, 0, kFormatBytes, "BYTES", 64, slot_count, "xreal_raw_hid", "Experimental native C++ raw XREAL HID packet"};
}

StreamSpec make_xreal_imu_stream(uint32_t slot_count) {
  return StreamSpec{"imu0", kKindImu, "IMU", 0, 0, kFormatImuF32Le, "IMU_F32_LE", 24, slot_count, "imu0", "Experimental native C++ normalized IMU sample"};
}

}  // namespace xr_capture_cpp
