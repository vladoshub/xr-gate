#include "sensor_source.hpp"
#include "runtime_tcp_client.hpp"
#include <algorithm>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <xr_runtime/contracts/runtime_adapter.hpp>
#include <xr_spatial/contracts/spatial_mesh_wire.hpp>
#include <xr_video/net/stereo_video_tcp.hpp>
#ifndef _WIN32
#include <fcntl.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
namespace xr::monado_driver {
bool option(const char *name, bool fallback) {
  const char *p = std::getenv(name);
  if (!p || !*p)
    return fallback;
  return !std::strcmp(p, "1") || !std::strcmp(p, "true") ||
         !std::strcmp(p, "yes") || !std::strcmp(p, "on");
}
std::string setting(const char *name, const char *fallback) {
  const char *p = std::getenv(name);
  return p && *p ? p : fallback;
}
uint32_t number(const char *name, uint32_t fallback, uint32_t min,
                uint32_t max) {
  auto text = setting(name, "");
  if (text.empty())
    return fallback;
  char *end = nullptr;
  const auto n = std::strtoul(text.c_str(), &end, 10);
  if (!end || *end || n < min || n > max)
    throw std::runtime_error(std::string("invalid ") + name);
  return static_cast<uint32_t>(n);
}
namespace {
constexpr size_t max_payload = 32u * 1024 * 1024;
#ifndef _WIN32
// pread avoids SIGBUS when a publisher truncates an open object during restart.
// Read the slot's seqlock before and after copying, and re-resolve registry on
// reopen.
class Ring {
public:
  std::string frame_id = "runtime_local";
  ~Ring() { close(); }
  bool read(const char *env_name, const char *fallback,
            const char *registry_env, const char *registry,
            const char *stream_env, const char *stream, const char (&magic)[8],
            std::vector<uint8_t> &bytes) {
    const auto now = monotonic_now_ns();
    struct stat st{};
    if (fd_ >= 0 && (::fstat(fd_, &st) || !st.st_nlink || st.st_size != size_ ||
                     now - progress_ > 500000000LL))
      close();
    if (fd_ < 0) {
      if (now < retry_)
        return false;
      retry_ = now + 100000000LL;
      auto name = setting(env_name, "");
      if (name.empty()) {
        try {
          std::ifstream in(setting(registry_env, registry));
          nlohmann::json j;
          in >> j;
          const auto &meta = j.at("streams").at(setting(stream_env, stream));
          name = meta.at("shm_name").get<std::string>();
          // Geometry must be normalized by xr_runtime_adapter before spatial
          // exposure.
          frame_id = meta.value("frame_id", "runtime_local");
        } catch (...) {
          name = fallback;
        }
      }
      if (name.empty())
        return false;
      if (name[0] != '/')
        name = '/' + name;
      fd_ = ::shm_open(name.c_str(), O_RDONLY | O_CLOEXEC, 0);
      if (fd_ < 0)
        return false;
      if (::fstat(fd_, &st) || !st.st_nlink) {
        close();
        return false;
      }
      size_ = st.st_size;
      progress_ = now;
    }
    xr_runtime::RingHeaderV1 h{};
    if (::pread(fd_, &h, sizeof(h), 0) != sizeof(h) ||
        std::memcmp(h.magic, magic, 8) || h.version != 1 ||
        h.header_size < sizeof(h) || h.slot_header_size < 40 || !h.slot_count ||
        h.payload_size > max_payload ||
        h.slot_stride < uint64_t(h.slot_header_size) + h.payload_size ||
        h.header_size > size_ ||
        h.slot_count > (uint64_t(size_) - h.header_size) / h.slot_stride) {
      close();
      return false;
    }
    if (!h.latest_sequence || h.latest_sequence == last_)
      return false;
    const off_t at = h.header_size + (h.latest_sequence - 1) % h.slot_count *
                                         uint64_t(h.slot_stride);
    // Spatial and video rings share the initial seq_begin, seq_end and size at
    // byte 32.
    struct Slot {
      uint64_t begin, end, ts, source;
      uint32_t payload;
    } slot{}, check{};
    if (::pread(fd_, &slot, sizeof(slot), at) != sizeof(slot) ||
        slot.begin != slot.end || slot.begin != h.latest_sequence * 2 ||
        !slot.payload || slot.payload > h.payload_size)
      return false;
    bytes.resize(slot.payload);
    if (::pread(fd_, bytes.data(), bytes.size(), at + h.slot_header_size) !=
            static_cast<ssize_t>(bytes.size()) ||
        ::pread(fd_, &check, sizeof(check), at) != sizeof(check) ||
        check.begin != slot.begin || check.end != slot.end)
      return false;
    if (last_ != h.latest_sequence) {
      last_ = h.latest_sequence;
      progress_ = now;
    }
    return true;
  }

private:
  void close() {
    if (fd_ >= 0)
      ::close(fd_);
    fd_ = -1;
    last_ = 0;
    size_ = 0;
  }
  int fd_ = -1;
  off_t size_ = 0;
  uint64_t last_ = 0;
  int64_t progress_ = 0, retry_ = 0;
};
#endif
int64_t nonnegative_age(uint64_t timestamp, int64_t now) {
  if (!timestamp || timestamp > uint64_t(now) + 1000000000ULL)
    return INT64_MAX;
  return timestamp > uint64_t(now) ? 0 : now - static_cast<int64_t>(timestamp);
}
} // namespace
SensorSource::SensorSource()
    : spatial_(option("XR_MONADO_SPATIAL")), video_(option("XR_MONADO_VIDEO")) {
  worker_ = std::thread([this] { run(); });
  if (video_ && setting("XR_MONADO_VIDEO_TRANSPORT", "shm") == "tcp")
    video_worker_ = std::thread([this] { run_video_tcp(); });
}
SensorSource::~SensorSource() {
  stop_ = true;
  if (worker_.joinable())
    worker_.join();
  if (video_worker_.joinable())
    video_worker_.join();
}
SensorState SensorSource::latest() {
  std::lock_guard<std::mutex> lock(mutex_);
  auto out = state_;
  const auto now = monotonic_now_ns();
  if (now - mesh_local_ns_ >
      int64_t(number("XR_MONADO_SPATIAL_MAX_AGE_MS", 1500)) * 1000000)
    out.mesh.reset();
  out.mesh_world_valid =
      out.mesh && out.tracking_valid && mesh_epoch_ == out.tracking_epoch;
  if (now - video_local_ns_ >
      int64_t(number("XR_MONADO_VIDEO_MAX_AGE_MS", 250)) * 1000000)
    out.video.reset();
  return out;
}
void SensorSource::run() {
  try {
    RuntimePoseReaderConfig cfg;
    cfg.transport = setting("XR_MONADO_TRANSPORT", "shm");
    cfg.shm_name = setting("XR_MONADO_HMD_SHM", "runtime_hmd_pose");
    cfg.tcp.host = setting("XR_MONADO_TCP_HOST", "127.0.0.1");
    cfg.tcp.port = uint16_t(number("XR_MONADO_TCP_PORT", 45810, 1, 65535));
    cfg.tcp.reconnect_ms = number("XR_MONADO_TCP_RECONNECT_MS", 500);
    cfg.tcp.timeout_ms = number("XR_MONADO_TCP_TIMEOUT_MS", 1000);
    auto pose = create_runtime_pose_reader(cfg);
    if (!pose)
      throw std::runtime_error("unsupported Monado transport");
    const bool mesh_tcp = spatial_ && setting("XR_MONADO_SPATIAL_TRANSPORT",
                                              cfg.transport.c_str()) == "tcp";
    auto tcp = mesh_tcp ? xr::openvr_driver::RuntimeTcpClient::acquire(cfg.tcp)
                        : nullptr;
#ifndef _WIN32
    Ring mesh_ring, video_ring;
#endif
    bool was_valid = false;
    int64_t epoch_begin = 0;
    const bool video_shm =
        video_ && setting("XR_MONADO_VIDEO_TRANSPORT", "shm") == "shm";
    while (!stop_) {
      RuntimePoseSample p;
      std::string error;
      const auto now = monotonic_now_ns();
      const bool valid = pose->read_latest(p, &error) &&
                         runtime_pose_is_valid(p.pose) &&
                         runtime_pose_sample_is_fresh(p, 250);
      std::shared_ptr<const xr_spatial::RuntimeSpatialProxyMeshF32V1> mesh;
      std::shared_ptr<const xr_video::StereoVideoFrame> video;
      int64_t mesh_time = 0, video_time = 0;
      bool world_mesh = true;
      if (valid && !was_valid) {
        epoch_begin = now;
        std::lock_guard<std::mutex> lock(mutex_);
        ++state_.tracking_epoch;
        state_.mesh.reset();
      }
      was_valid = valid;
      std::vector<uint8_t> data;
      if (tcp) {
        xr::openvr_driver::RuntimeTcpSnapshot snap;
        if (tcp->read_latest(snap, &error) && snap.spatial) {
          mesh = snap.spatial;
          const auto age = std::min<int64_t>(snap.spatial_age_ns, now);
          mesh_time = snap.spatial_received_ns - age;
        }
      }
#ifndef _WIN32
      if (spatial_ && !mesh_tcp) {
        const char magic[8] = {'S', 'P', 'M', 'S', 'H', 'M', '1', '\0'};
        if (mesh_ring.read("XR_MONADO_SPATIAL_SHM",
                           "runtime_spatial_proxy_mesh",
                           "XR_MONADO_SPATIAL_REGISTRY",
                           "/tmp/runtime_tracking_streams.json",
                           "XR_MONADO_SPATIAL_STREAM",
                           "runtime_spatial_proxy_mesh", magic, data) &&
            data.size() == sizeof(xr_spatial::RuntimeSpatialProxyMeshF32V1)) {
          auto m = std::make_shared<xr_spatial::RuntimeSpatialProxyMeshF32V1>();
          std::memcpy(m.get(), data.data(), data.size());
          if (xr_spatial::mesh_layout_valid(*m)) {
            mesh = m;
            mesh_time =
                now - std::min(nonnegative_age(m->timestamp_ns, now), now);
            world_mesh = mesh_ring.frame_id == "runtime_local" &&
                         setting("XR_MONADO_SPATIAL_FRAME", "runtime_local") ==
                             "runtime_local";
          }
        }
      }
      if (video_shm) {
        const char magic[8] = {'X', 'S', 'V', 'S', 'H', 'M', '1', '\0'};
        if (video_ring.read("XR_MONADO_VIDEO_SHM", "runtime_stereo_video",
                            "XR_MONADO_VIDEO_REGISTRY",
                            "/tmp/runtime_video_streams.json",
                            "XR_MONADO_VIDEO_STREAM", "runtime_stereo_video",
                            magic, data)) {
          try {
            auto f = xr_video::read_frame_payload(data.data(), data.size());
            if (f.header.flags & xr_video::STEREO_VIDEO_FLAG_VALID) {
              video_time =
                  now -
                  std::min(nonnegative_age(f.header.timestamp_ns, now), now);
              video = std::make_shared<const xr_video::StereoVideoFrame>(
                  std::move(f));
            }
          } catch (const std::exception &) {
          }
        }
      }
#endif
      {
        std::lock_guard<std::mutex> lock(mutex_);
        state_.tracking_valid = valid;
        // Never relabel a pre-reset map as belonging to a new tracking epoch.
        if (mesh) {
          state_.mesh = std::move(mesh);
          mesh_local_ns_ = mesh_time;
          mesh_epoch_ = world_mesh && valid && mesh_time >= epoch_begin
                            ? state_.tracking_epoch
                            : UINT64_MAX;
        }
        if (video) {
          state_.video = std::move(video);
          video_local_ns_ = video_time;
        }
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  } catch (const std::exception &e) {
    std::fprintf(stderr, "[xr_gate_data] %s\n", e.what());
  }
}
void SensorSource::run_video_tcp() {
#ifndef _WIN32
  // Nonblocking IPv4 connect/receive with bounded waits; all work stays on this
  // optional worker.
  while (!stop_) {
    int fd = -1;
    try {
      auto host = setting("XR_MONADO_VIDEO_TCP_HOST", "127.0.0.1");
      if (host == "localhost")
        host = "127.0.0.1";
      sockaddr_in address{};
      address.sin_family = AF_INET;
      address.sin_port =
          htons(uint16_t(number("XR_MONADO_VIDEO_TCP_PORT", 45812, 1, 65535)));
      if (::inet_pton(AF_INET, host.c_str(), &address.sin_addr) != 1)
        throw std::runtime_error("video TCP host must be IPv4 or localhost");
      fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
      if (fd < 0)
        throw std::runtime_error("video socket failed");
      auto transfer = [&](uint8_t *dst, size_t size) {
        const auto deadline = monotonic_now_ns() + 1000000000LL;
        size_t off = 0;
        while (off < size && !stop_ && monotonic_now_ns() < deadline) {
          pollfd p{fd, POLLIN, 0};
          if (::poll(&p, 1, 50) <= 0)
            continue;
          const auto n = ::recv(fd, dst + off, size - off, 0);
          if (n == 0)
            throw std::runtime_error("video disconnected");
          if (n < 0) {
            if (errno == EAGAIN || errno == EINTR)
              continue;
            throw std::runtime_error("video recv failed");
          }
          off += size_t(n);
        }
        if (off < size)
          throw std::runtime_error("video frame timeout");
      };
      if (::connect(fd, reinterpret_cast<sockaddr *>(&address),
                    sizeof(address)) != 0) {
        if (errno != EINPROGRESS)
          throw std::runtime_error("video connect failed");
        bool done = false;
        for (int i = 0; i < 20 && !stop_; ++i) {
          pollfd p{fd, POLLOUT, 0};
          if (::poll(&p, 1, 50) > 0) {
            int e = 0;
            socklen_t n = sizeof(e);
            if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &e, &n) || e)
              throw std::runtime_error("video connect failed");
            done = true;
            break;
          }
        }
        if (!done)
          throw std::runtime_error("video connect timeout");
      }
      while (!stop_) {
        xr_video::StereoVideoNetFrameHeaderV1 net;
        transfer(reinterpret_cast<uint8_t *>(&net), sizeof(net));
        if (std::memcmp(net.magic, "XSVNET1", 8) || net.version != 1 ||
            net.header_size != sizeof(net) ||
            net.frame_header_size !=
                sizeof(xr_video::StereoVideoFrameHeaderV1) ||
            uint64_t(net.left_size_bytes) + net.right_size_bytes >
                max_payload - sizeof(xr_video::StereoVideoFrameHeaderV1))
          throw std::runtime_error("invalid video TCP header");
        std::vector<uint8_t> bytes(sizeof(xr_video::StereoVideoFrameHeaderV1) +
                                   net.left_size_bytes + net.right_size_bytes);
        transfer(bytes.data(), bytes.size());
        auto frame = xr_video::read_frame_payload(bytes.data(), bytes.size());
        if (frame.header.sequence != net.sequence ||
            frame.header.left_size_bytes != net.left_size_bytes ||
            frame.header.right_size_bytes != net.right_size_bytes)
          throw std::runtime_error("inconsistent video TCP frame");
        const uint64_t row = uint64_t(frame.header.width) *
                             xr_video::bytes_per_pixel(
                                 static_cast<xr_video::StereoVideoPixelFormat>(
                                     frame.header.pixel_format));
        if (frame.header.stride_left < row || frame.header.stride_right < row ||
            uint64_t(frame.header.stride_left) * frame.header.height >
                frame.left.size() ||
            uint64_t(frame.header.stride_right) * frame.header.height >
                frame.right.size())
          throw std::runtime_error("invalid video stride");
        // Sender/source clock differences do not affect local expiry. Existing
        // video protocol has no server-now field; no end-to-end age is
        // inferred.
        std::lock_guard<std::mutex> lock(mutex_);
        if (frame.header.flags & xr_video::STEREO_VIDEO_FLAG_VALID) {
          state_.video = std::make_shared<const xr_video::StereoVideoFrame>(
              std::move(frame));
          video_local_ns_ = monotonic_now_ns();
        } else
          state_.video.reset();
      }
    } catch (const std::exception &) {
      std::lock_guard<std::mutex> lock(mutex_);
      state_.video.reset();
    }
    if (fd >= 0)
      ::close(fd);
    for (int i = 0; i < 25 && !stop_; ++i)
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
#endif
}
} // namespace xr::monado_driver
