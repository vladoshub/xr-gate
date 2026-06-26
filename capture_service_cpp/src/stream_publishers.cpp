#include "capture_service_cpp/stream_publishers.hpp"

#include "capture_service_cpp/platform/capabilities.hpp"
#include "capture_service_cpp/platform/process.hpp"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <stdexcept>

#include <cstdio>

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

void StreamPublishers::add_stream(const StreamSpec& spec) {
  specs_[spec.stream_id] = spec;
  sequences_.emplace(spec.stream_id, 0);
  if (enable_shm_) shm_publishers_[spec.stream_id] = std::make_unique<ShmPublisher>(spec, cfg_.namespace_name);
}

void StreamPublishers::start() {
  if (enable_tcp_) {
    tcp_ = std::make_unique<TcpFanoutPublisher>(specs_, cfg_.tcp_bind_host, cfg_.tcp_port, cfg_.namespace_name, cfg_.tcp_client_queue_size);
    tcp_->start();
  }
}

void StreamPublishers::write_registry() {
  std::ostringstream os;
  os << "{\n"
     << "  \"config_path\": \"" << json_escape(cfg_.config_path) << "\",\n"
     << "  \"created_unix_ns\": " << wall_ns() << ",\n"
     << "  \"namespace\": \"" << json_escape(cfg_.namespace_name) << "\",\n"
     << "  \"pid\": " << current_process_id() << ",\n"
     << "  \"publish\": [";
  bool first_mode = true;
  if (enable_shm_) { os << "\"shm\""; first_mode = false; }
  if (enable_tcp_) { if (!first_mode) os << ", "; os << "\"tcp\""; }
  os << "],\n"
     << "  \"service\": \"capture_service\",\n"
     << "  \"implementation\": \"capture_service_cpp_experimental\",\n"
     << "  \"streams\": {\n";

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
  if (std::rename(tmp.c_str(), cfg_.registry_path.c_str()) != 0) {
    throw std::runtime_error("failed to replace registry: " + cfg_.registry_path);
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
  auto shm_it = shm_publishers_.find(stream_id);
  if (shm_it != shm_publishers_.end()) {
    seq = shm_it->second->publish(payload, payload_len, timestamp_ns, width, height);
    sequences_[stream_id] = seq;
  } else {
    seq = ++sequences_[stream_id];
  }
  if (tcp_) tcp_->publish(stream_id, payload, payload_len, seq, timestamp_ns, width, height, format_code, flags, frame_id);
  return seq;
}

}  // namespace xr_capture_cpp
