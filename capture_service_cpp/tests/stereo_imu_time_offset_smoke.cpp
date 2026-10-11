#include <capture_client/sync/synchronizer.hpp>

#include <limits>
#include <stdexcept>

using namespace capture_client;
namespace {
constexpr int64_t ms = 1000000;
void require(bool value) {
  if (!value) throw std::runtime_error("stereo/IMU offset check failed");
}
class Reader final : public IStreamReader {
 public:
  const StreamInfo& info() const override { return info_; }
  uint64_t latest_sequence() const override { return messages.size(); }
  bool read_latest(RawMessage& out) const override { return read_sequence(messages.size(), out); }
  bool read_sequence(uint64_t n, RawMessage& out) const override {
    if (!n || n > messages.size()) return false;
    out = messages[n - 1];
    return true;
  }
  void push(int64_t timestamp, bool image) {
    RawMessage msg;
    msg.sequence = messages.size() + 1;
    msg.timestamp_ns = timestamp;
    msg.format_code = static_cast<uint32_t>(image ? FormatCode::GRAY8 : FormatCode::IMU_F32_LE);
    msg.width = msg.height = image ? 1 : 0;
    msg.payload_size = image ? 1 : 24;
    msg.payload.resize(msg.payload_size);
    messages.push_back(msg);
  }
  std::vector<RawMessage> messages;
 private:
  StreamInfo info_;
};
class Transport final : public ICaptureTransport {
 public:
  const std::string& type() const override { return type_; }
  IStreamReader& cam0() override { return left; }
  IStreamReader& cam1() override { return right; }
  IStreamReader& imu() override {
    if (forbid_imu) throw std::runtime_error("VO touched IMU");
    return inertial;
  }
  void frame(int64_t time) { left.push(time, true); right.push(time, true); }
  Reader left, right, inertial;
  bool forbid_imu = false;
 private:
  std::string type_ = "test";
};
}

int main() {
  Transport positive;
  StereoImuSynchronizer sync(positive, ms, 0.0, true, 3 * ms, true);
  positive.frame(20 * ms);
  for (int64_t t : {18, 20, 22, 23, 24, 25}) positive.inertial.push(t * ms, false);
  auto packet = sync.read_next(0.02);
  require(packet && packet->pair.timestamp_ns == 23 * ms);  // Shift once, not twice.
  require(packet->pair.cam0.timestamp_ns == 20 * ms);  // Original image stamp retained.
  require(packet->imu_samples.size() == 5 && packet->imu_samples.back().timestamp_ns == 24 * ms);
  require(packet->latest_imu_delta_ms() == 1.0);
  positive.frame(30 * ms);
  for (int64_t t : {28, 30, 33, 34, 35}) positive.inertial.push(t * ms, false);
  packet = sync.read_next(0.02);
  require(packet && packet->pair.timestamp_ns == 33 * ms);
  require(packet->previous_camera_timestamp_ns == 23 * ms);
  require(packet->imu_samples.front().timestamp_ns == 25 * ms);  // No sample lost/duplicated.
  require(packet->imu_samples.back().timestamp_ns == 34 * ms);

  Transport negative;
  StereoImuSynchronizer negative_sync(negative, ms, 0.0, true, -3 * ms, true);
  negative.frame(20 * ms);
  for (int64_t t : {16, 17, 18, 19, 20, 21}) negative.inertial.push(t * ms, false);
  packet = negative_sync.read_next(0.02);
  require(packet && packet->pair.timestamp_ns == 17 * ms);
  require(packet->imu_samples.size() == 3 && packet->imu_samples.back().timestamp_ns == 18 * ms);

  Transport legacy;
  StereoImuSynchronizer legacy_sync(legacy, ms, 0.0);
  legacy.frame(20 * ms);
  for (int64_t t : {19, 20, 21}) legacy.inertial.push(t * ms, false);
  packet = legacy_sync.read_next(0.02);
  require(packet && packet->pair.timestamp_ns == 20 * ms && packet->imu_samples.size() == 2);

  Transport vo;
  vo.forbid_imu = true;
  StereoImuSynchronizer vo_sync(vo, ms, 0.0, false, 0, true);
  vo.frame(20 * ms);
  packet = vo_sync.read_next(0.02);
  require(packet && packet->pair.timestamp_ns == 20 * ms && packet->imu_samples.empty());

  bool overflow = false;
  try { shift_camera_timestamp_ns(std::numeric_limits<int64_t>::max(), 1); }
  catch (const std::overflow_error&) { overflow = true; }
  require(overflow);
  bool underflow = false;
  try { shift_camera_timestamp_ns(std::numeric_limits<int64_t>::min(), -1); }
  catch (const std::overflow_error&) { underflow = true; }
  require(underflow);
  bool negative_time = false;
  try { shift_camera_timestamp_ns(1, -2); }
  catch (const std::runtime_error&) { negative_time = true; }
  require(negative_time);
  return 0;
}
