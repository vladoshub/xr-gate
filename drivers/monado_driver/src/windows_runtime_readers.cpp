#include "xr_monado_driver/runtime_readers.hpp"

#ifdef _WIN32

#include <memory>
#include <string>

namespace xr::monado_driver {
namespace {

class UnimplementedPoseReader final : public IRuntimePoseReader {
 public:
  bool read_latest(RuntimePoseSample&, std::string* error) override {
    if (error) *error = "Windows runtime pose transport is not implemented yet";
    return false;
  }
  const char* transport_name() const override { return "windows_unimplemented"; }
};

class UnimplementedControllerStateReader final : public IRuntimeControllerStateReader {
 public:
  bool read_latest(RuntimeControllerStateSample&, std::string* error) override {
    if (error) *error = "Windows runtime controller transport is not implemented yet";
    return false;
  }
  const char* transport_name() const override { return "windows_unimplemented"; }
};

class UnimplementedHandReader final : public IRuntimeHandReader {
 public:
  bool read_latest(RuntimeHandSample&, std::string* error) override {
    if (error) *error = "Windows runtime hand transport is not implemented yet";
    return false;
  }
  const char* transport_name() const override { return "windows_unimplemented"; }
};

}  // namespace

std::unique_ptr<IRuntimePoseReader> create_windows_runtime_pose_reader(const RuntimePoseReaderConfig&) {
  return std::make_unique<UnimplementedPoseReader>();
}

std::unique_ptr<IRuntimeControllerStateReader> create_windows_runtime_controller_state_reader(
    const RuntimeControllerStateReaderConfig&) {
  return std::make_unique<UnimplementedControllerStateReader>();
}

std::unique_ptr<IRuntimeHandReader> create_windows_runtime_hand_reader(const RuntimeHandReaderConfig&) {
  return std::make_unique<UnimplementedHandReader>();
}

}  // namespace xr::monado_driver

#endif  // _WIN32
