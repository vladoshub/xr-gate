// Exercise production helpers and map code, with in-memory camera readers.
#define main xr_spatial_backend_main
#include "../src/xr_spatial_backend.cpp"
#undef main

namespace {
void check(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

class ImageReader final : public capture_client::IStreamReader {
 public:
  std::vector<capture_client::ImageFrame> frames;
  capture_client::StreamInfo metadata;
  mutable size_t image_reads = 0;
  mutable uint64_t fail_once_sequence = 0;
  const capture_client::StreamInfo& info() const override { return metadata; }
  uint64_t latest_sequence() const override { return frames.size(); }
  bool read_latest(capture_client::RawMessage&) const override { return false; }
  bool read_sequence(uint64_t, capture_client::RawMessage&) const override { return false; }
  bool read_image_sequence(uint64_t seq, capture_client::ImageFrame& out) const override {
    ++image_reads;
    if (seq == fail_once_sequence) { fail_once_sequence = 0; return false; }
    if (seq == 0 || seq > frames.size()) return false;
    out = frames[seq - 1];
    return true;
  }
};

class Capture final : public capture_client::ICaptureTransport {
 public:
  ImageReader a, b;
  capture_client::NullStreamReader no_imu;
  const std::string& type() const override { static const std::string value = "test"; return value; }
  capture_client::IStreamReader& cam0() override { return a; }
  capture_client::IStreamReader& cam1() override { return b; }
  capture_client::IStreamReader& imu() override { return no_imu; }
};
}

int main() {
  try {
    using namespace xr_spatial_backend;
    PointCloudSpatialMap map(1000);
    PointCloudFrame cloud;
    cloud.points.assign(100, {0.0f, 0.0f, 1.0f});
    for (int frame = 0; frame < 12; ++frame) {
      Transform3d transform;
      transform.t.x = frame;
      map.integrate(cloud, transform);
    }
    check(map.points_tracking().size() == 1000, "reservoir exceeds point budget");
    check(map.integrated_frames() == 12 && map.observed_points() == 1200, "lost observation counters");
    check(std::any_of(map.points_tracking().begin(), map.points_tracking().end(),
                      [](const Vec3f& p) { return p.x >= 10.0f; }), "new scan area lost after point cap");
    map.reset();
    check(map.points_tracking().empty() && map.observed_points() == 0 && map.integrated_frames() == 0,
          "map reset must clear sampling history");
    cloud.points = {{std::numeric_limits<float>::quiet_NaN(), 0, 1}};
    map.integrate(cloud, {});
    check(map.integrated_frames() == 0 && map.dropped_frames() == 1, "invalid cloud counted as integrated");

    Capture capture;
    for (int frame = 0; frame < 8; ++frame) {
      capture_client::ImageFrame image;
      image.sequence = frame + 1;
      image.timestamp_ns = 1000000000LL + frame * 16666667LL;
      image.gray8 = {static_cast<uint8_t>(frame)};
      capture.a.frames.push_back(image);
      capture.b.frames.push_back(image);
    }
    for (const int64_t offset : {-28111831LL, -1830051LL, 9000000LL, 0LL}) {
      const auto target = capture.a.frames.back().timestamp_ns + offset;
      StereoPairReader reader(16, 1000000, offset);
      const auto* selected = reader.read(capture, target);
      check(selected && selected->sequence == 8 && selected->pair.timestamp_ns == target,
            "camera/IMU offset did not select the matching frame");
      check(selected->pair.cam0.timestamp_ns == capture.a.frames.back().timestamp_ns,
            "source image timestamp was shifted twice");
    }

    capture.a.image_reads = capture.b.image_reads = 0;
    StereoPairReader reader(4, 1000000);
    const auto* selected = reader.read(capture, capture.a.frames.back().timestamp_ns);
    check(selected && selected->sequence == 8, "initial timestamp selection failed");
    const auto* pixels = selected->pair.cam0.gray8.data();
    for (int poll = 0; poll < 1000; ++poll) {
      selected = reader.read(capture, capture.a.frames.back().timestamp_ns);
      check(selected && selected->pair.cam0.gray8.data() == pixels,
            "polling copied or replaced an immutable frame");
    }
    selected = reader.read(capture, capture.a.frames[4].timestamp_ns);
    check(selected && selected->sequence == 5 && capture.a.image_reads == 4 && capture.b.image_reads == 4,
          "pose changes must select cached history without rereading images");
    auto next_image = capture.a.frames.back();
    ++next_image.sequence;
    next_image.timestamp_ns += 16666667LL;
    capture.a.frames.push_back(next_image);
    capture.b.frames.push_back(next_image);
    selected = reader.read(capture, next_image.timestamp_ns);
    check(selected && selected->sequence == 9 && capture.a.image_reads == 5 && capture.b.image_reads == 5,
          "a new sequence must read only the new stereo pair");
    selected = reader.read(capture, capture.a.frames[4].timestamp_ns);
    check(selected && selected->sequence == 6, "cache retained a frame outside the search window");

    // A replacement SHM can reuse the same sequences with different contents.
    for (auto* camera : {&capture.a, &capture.b}) {
      camera->frames.back().timestamp_ns += 1000000000LL;
      camera->frames.back().gray8 = {99};
    }
    reader.reset();
    selected = reader.read(capture, capture.a.frames.back().timestamp_ns);
    check(selected && selected->pair.cam0.gray8.front() == 99,
          "capture reattach kept pixels from the previous generation");
    capture.a.frames.resize(2);
    capture.b.frames.resize(2);
    for (auto* camera : {&capture.a, &capture.b}) camera->frames.back().gray8 = {42};
    selected = reader.read(capture, capture.a.frames.back().timestamp_ns);
    check(selected && selected->sequence == 2 && selected->pair.cam0.gray8.front() == 42,
          "camera sequence restart kept stale cached frames");
    reader.reset();
    capture.b.fail_once_sequence = 2;
    selected = reader.read(capture, capture.a.frames.back().timestamp_ns);
    check(selected && selected->sequence == 1, "unreadable pair did not fall back to history");
    selected = reader.read(capture, capture.a.frames.back().timestamp_ns);
    check(selected && selected->sequence == 2, "a transient read failure was cached permanently");
    StereoPairReader camera_only(1, 1000000);
    selected = camera_only.read(capture);
    check(selected && selected->sequence == 2, "camera-only mode did not choose the newest pair");
    capture.b.frames.back().timestamp_ns += 2000000LL;
    reader.reset();
    const size_t reads_before_mismatch = capture.a.image_reads;
    for (int poll = 0; poll < 100; ++poll) {
      selected = reader.read(capture, capture.a.frames.back().timestamp_ns);
      check(selected && selected->sequence == 1, "unsynchronized stereo pair was selected");
    }
    check(capture.a.image_reads == reads_before_mismatch + 2,
          "a persistent camera timestamp mismatch causes repeated image copies");

    xr_runtime::HmdPoseF64V1 pose{};
    pose.timestamp_ns = 1;
    pose.flags = xr_runtime::HMD_FLAG_POSE_VALID;
    check(!pose_is_usable(pose), "zero quaternion accepted");
    pose.qw = 2.0;
    check(pose_is_usable(pose), "normalizable quaternion rejected");
    auto other = pose;
    other.qw = 0.7071067811865476;
    other.qz = 0.7071067811865476;
    check(std::abs(pose_rotation_delta_deg(pose, other) - 90.0) < 1e-6,
          "pose jump comparison requires normalized quaternions");
    pose.px = std::numeric_limits<double>::infinity();
    check(!pose_is_usable(pose), "non-finite pose accepted");

#ifdef __linux__
    cloud.points = {{0,0,1}};
    map.integrate(cloud, {});
    bool write_failed = false;
    try { map.save_pointcloud_ply("/dev/full"); }
    catch (const std::exception&) { write_failed = true; }
    check(write_failed, "full output device reported as saved");
#endif
    std::cout << "spatial scanner core checks passed\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "scanner regression: " << e.what() << '\n';
    return 1;
  }
}
