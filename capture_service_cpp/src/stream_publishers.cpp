#include "capture_service_cpp/stream_publishers.hpp"

#include "capture_service_cpp/platform/capabilities.hpp"
#include "capture_service_cpp/platform/process.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace xr_capture_cpp {

StreamPublishers::StreamPublishers(RuntimeConfig cfg) : cfg_(std::move(cfg)) {
  enable_shm_ = std::find(cfg_.publish_modes.begin(), cfg_.publish_modes.end(), "shm") != cfg_.publish_modes.end();
  enable_tcp_ = std::find(cfg_.publish_modes.begin(), cfg_.publish_modes.end(), "tcp") != cfg_.publish_modes.end();
  if (enable_shm_ && !platform_supports_shm_publish()) {
    std::cerr << "[capture_service_cpp][WARN] disabling SHM on this platform; TCP remains enabled" << std::endl;
    enable_shm_ = false;
  }
  if (platform_requires_tcp_publish() && !enable_tcp_) enable_tcp_ = true;
}

StreamPublishers::~StreamPublishers() = default;

void StreamPublishers::add_stream(const StreamSpec& spec, bool reserve_shm_storage) {
  if (enable_shm_) {
    shm_publishers_[spec.stream_id] = std::make_unique<ShmPublisher>(spec, cfg_.namespace_name, reserve_shm_storage);
  }
  specs_[spec.stream_id] = spec;
  sequences_.emplace(spec.stream_id, 0);
}

void StreamPublishers::add_stereo_pair(const std::string& name, const std::string& left, const std::string& right) {
  if (left == right || !specs_.count(left) || !specs_.count(right)) {
    throw std::runtime_error("stereo pair " + name + " needs two registered streams");
  }
  stereo_pairs_.emplace(name, std::array<std::string, 2>{left, right});
}

void StreamPublishers::start() {
  if (enable_tcp_) {
    tcp_ = std::make_unique<TcpFanoutPublisher>(specs_,
                                                        cfg_.tcp_bind_host,
                                                        cfg_.tcp_port,
                                                        cfg_.namespace_name,
                                                        cfg_.profile_name,
                                                        cfg_.tcp_client_queue_size);
    tcp_->start();
  }
}

std::vector<std::string> StreamPublishers::shm_names() const {
  std::vector<std::string> names;
  names.reserve(shm_publishers_.size());
  for (const auto& entry : shm_publishers_) names.push_back(entry.second->shm_name());
  return names;
}

void StreamPublishers::write_registry() {
  std::lock_guard<std::mutex> lock(registry_mutex_);
  write_registry_locked();
}

void StreamPublishers::set_tcp_imu_available(bool available) {
  std::lock_guard<std::mutex> lock(registry_mutex_);
  tcp_imu_available_ = available;
  stereo_ready_.store(false);
  imu_ready_.store(false);
  write_registry_locked();
}

void StreamPublishers::mark_tcp_capture_ready(uint32_t kind) {
  auto& ready = kind == kKindImage ? stereo_ready_ : imu_ready_;
  if (ready.load()) return;
  std::lock_guard<std::mutex> lock(registry_mutex_);
  if (kind == kKindImu && tcp_imu_available_ == false) return;
  if (ready.exchange(true)) return;
  write_registry_locked();
}

void StreamPublishers::write_registry_locked() {
  std::ostringstream os;
  os << "{\n"
     << "  \"config_path\": \"" << json_escape(cfg_.config_path) << "\",\n"
     << "  \"created_unix_ns\": " << wall_ns() << ",\n"
     << "  \"namespace\": \"" << json_escape(cfg_.namespace_name) << "\",\n"
     << "  \"profile\": \"" << json_escape(cfg_.profile_name) << "\",\n"
     << "  \"pid\": " << current_process_id() << ",\n"
     << "  \"publish\": [";
  bool first_mode = true;
  if (enable_shm_) { os << "\"shm\""; first_mode = false; }
  if (enable_tcp_) { if (!first_mode) os << ", "; os << "\"tcp\""; }
  os << "],\n"
     << "  \"service\": \"capture_service\",\n"
     << "  \"implementation\": \"capture_service_cpp_experimental\",\n";
  const bool tcp_camera = cfg_.camera.enabled && cfg_.camera.stereo_enabled && cfg_.camera.driver == "tcp";
  const bool tcp_imu = cfg_.imu.enabled && cfg_.imu.driver == "tcp";
  if (tcp_camera || tcp_imu) {
    const auto available = tcp_camera && tcp_imu ? tcp_imu_available_ : std::optional<bool>(cfg_.imu.enabled);
    os << "  \"tcp_input\": {\"camera\": " << (tcp_camera ? "true" : "false")
       << ", \"stereo_ready\": " << (stereo_ready_.load() ? "true" : "false")
       << ", \"imu_ready\": " << (imu_ready_.load() ? "true" : "false")
       << ", \"imu_available\": " << (available ? (*available ? "true" : "false") : "null")
       << ", \"imu_stream\": \"" << json_escape(cfg_.imu.stream_id) << "\"},\n";
  }
  if (!stereo_pairs_.empty()) {
    os << "  \"stereo_pairs\": {";
    bool first_pair = true;
    for (const auto& [name, cameras] : stereo_pairs_) {
      if (!first_pair) os << ",";
      first_pair = false;
      os << "\n    \"" << json_escape(name) << "\": [\"" << json_escape(cameras[0])
         << "\", \"" << json_escape(cameras[1]) << "\"]";
    }
    os << "\n  },\n";
  }
  os << "  \"streams\": {\n";

  size_t emitted = 0;
  for (const auto& kv : specs_) {
    const auto& sid = kv.first;
    const auto& spec = kv.second;
    if (emitted++) os << ",\n";
    os << "    \"" << sid << "\": ";
    auto shm_it = shm_publishers_.find(sid);
    if (shm_it != shm_publishers_.end()) {
      std::string entry = shm_it->second->registry_entry();
      if (enable_tcp_ && tcp_) {
        // Insert a Python-compatible secondary TCP transport object before the closing brace.
        const std::string suffix = "\n    }";
        const auto pos = entry.rfind(suffix);
        std::ostringstream tcp_extra;
        tcp_extra << ",\n"
                  << "      \"tcp\": {\n"
                  << "        \"protocol\": \"" << kTcpProtocolName << "\",\n"
                  << "        \"tcp_host\": \"" << tcp_->advertise_host() << "\",\n"
                  << "        \"tcp_port\": " << cfg_.tcp_port << ",\n"
                  << "        \"transport\": \"tcp\"\n"
                  << "      }";
        if (pos != std::string::npos) entry.insert(pos, tcp_extra.str());
      }
      os << entry;
    } else if (enable_tcp_ && tcp_) {
      os << tcp_->registry_entry(spec);
    } else {
      os << "{}";
    }
  }
  os << "\n  }\n}\n";

  const std::string tmp = cfg_.registry_path + ".tmp";
  std::ofstream f(tmp);
  if (!f) throw std::runtime_error("failed to write registry tmp: " + tmp);
  f << os.str();
  f.close();
  if (!f) throw std::runtime_error("failed to write registry tmp: " + tmp);
  std::error_code error;
  std::filesystem::rename(tmp, cfg_.registry_path, error);
  if (error) {
    throw std::runtime_error("failed to replace registry: " + cfg_.registry_path + ": " + error.message());
  }
  std::cerr << "[capture_service_cpp] registry: " << cfg_.registry_path << std::endl;
  for (const auto& kv : specs_) {
    const auto& sid = kv.first;
    auto shm_it = shm_publishers_.find(sid);
    if (shm_it != shm_publishers_.end()) std::cerr << "[capture_service_cpp] stream " << sid << ": shm=" << shm_it->second->shm_name() << std::endl;
    if (enable_tcp_) std::cerr << "[capture_service_cpp] stream " << sid << ": tcp=" << (tcp_ ? tcp_->advertise_host() : cfg_.tcp_bind_host) << ":" << cfg_.tcp_port << std::endl;
  }
}

uint64_t StreamPublishers::publish(const std::string& stream_id,
                                   const uint8_t* payload,
                                   size_t payload_len,
                                   uint64_t timestamp_ns,
                                   uint32_t width,
                                   uint32_t height,
                                   uint32_t format_code,
                                   uint32_t flags,
                                   const std::string& frame_id) {
  auto spec_it = specs_.find(stream_id);
  if (spec_it == specs_.end()) return 0;
  uint64_t seq = 0;
  auto& sequence = sequences_.at(stream_id);
  auto shm_it = shm_publishers_.find(stream_id);
  if (shm_it != shm_publishers_.end()) {
    seq = shm_it->second->publish(payload, payload_len, timestamp_ns, width, height);
    sequence = seq;
  } else {
    seq = ++sequence;
  }
  if (tcp_) tcp_->publish(stream_id, payload, payload_len, seq, timestamp_ns, width, height, format_code, flags, frame_id);
  return seq;
}

}  // namespace xr_capture_cpp
