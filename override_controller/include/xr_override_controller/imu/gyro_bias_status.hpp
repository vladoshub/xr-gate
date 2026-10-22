#pragma once

#include <xr_override_controller/imu/controller_imu_processor.hpp>
#include <cstdint>
#include <iostream>
#include <string>

namespace xr_override_controller::imu {

struct GyroBiasLogState {
  uint64_t last_log_ns = 0;
  uint32_t epoch = 0;
  bool ready = false;
};

inline void log_gyro_bias_status(const ControllerImuProcessor& processor,
    const std::string& device, uint32_t epoch, uint64_t now_ns, GyroBiasLogState& state) {
  const bool ready = processor.gyro_calibrated();
  const bool changed = state.epoch != epoch || state.ready != ready;
  if (!changed && (ready || (state.last_log_ns && now_ns >= state.last_log_ns &&
      now_ns - state.last_log_ns < 3'000'000'000ULL))) return;
  state = {now_ns, epoch, ready};
  const auto& bias = processor.gyro_bias_rad_s();
  std::cerr << "[override_controller][gyro-zero] device=" << device
            << " epoch=" << epoch << " state=" << (ready ? "ready" : "waiting")
            << " reason=" << processor.gyro_bias_wait_reason()
            << " stable_ms=" << processor.gyro_bias_stable_ms()
            << " required_ms=" << processor.gyro_bias_config().stable_ms
            << " samples=" << processor.gyro_bias_sample_count()
            << " bias_rad_s=(" << bias.x << ',' << bias.y << ',' << bias.z << ")\n";
}

}  // namespace xr_override_controller::imu
