#include "capture_service_cpp/shm_publisher.hpp"

#include <cstdio>
#include <fstream>
#include <stdexcept>

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace xr_capture_cpp {

#pragma pack(push, 1)
struct GlobalHeader {
  char magic[8];
  uint32_t version;
  uint32_t kind;
  uint32_t slot_count;
  uint32_t slot_header_size;
  uint32_t payload_size;
  uint32_t header_size;
  uint64_t created_ns;
  uint64_t latest_seq;
};

struct SlotHeaderWire {
  uint64_t seq_begin;
  uint64_t seq_end;
  int64_t timestamp_ns;
  uint64_t monotonic_ns;
  uint32_t payload_size;
  uint32_t width;
  uint32_t height;
  uint32_t format_code;
  uint32_t flags;
  uint32_t reserved;
  char frame_id[32];
};
#pragma pack(pop)

static std::string shm_posix_name(const std::string& name) {
  return (!name.empty() && name[0] == '/') ? name : "/" + name;
}

ShmPublisher::ShmPublisher(const StreamSpec& spec, const std::string& namespace_name)
    : spec_(spec), shm_name_(sanitize_shm_name(namespace_name + "_" + spec.stream_id)) {
  slot_stride_ = kSlotHeaderSize + spec.payload_size;
  size_ = kHeaderSize + spec.slot_count * slot_stride_;
  const std::string posix = shm_posix_name(shm_name_);
  ::shm_unlink(posix.c_str());
  fd_ = ::shm_open(posix.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600);
  if (fd_ < 0) throw std::runtime_error("shm_open failed for " + posix + ": " + std::strerror(errno));
  if (::ftruncate(fd_, static_cast<off_t>(size_)) != 0) {
    throw std::runtime_error("ftruncate failed for " + posix + ": " + std::strerror(errno));
  }
  data_ = static_cast<uint8_t*>(::mmap(nullptr, size_, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0));
  if (data_ == MAP_FAILED) throw std::runtime_error("mmap failed for " + posix + ": " + std::strerror(errno));
  write_global_header(0);
  write_metadata();
}

ShmPublisher::~ShmPublisher() {
  if (data_ && data_ != MAP_FAILED) ::munmap(data_, size_);
  if (fd_ >= 0) ::close(fd_);
}

uint64_t ShmPublisher::publish(const uint8_t* payload, size_t payload_len, uint64_t timestamp_ns, uint32_t width, uint32_t height) {
  if (payload_len > spec_.payload_size) throw std::runtime_error("payload too large for " + spec_.stream_id);
  const uint64_t seq = ++seq_;
  const size_t slot = (seq - 1) % spec_.slot_count;
  const size_t slot_off = kHeaderSize + slot * slot_stride_;
  const size_t payload_off = slot_off + kSlotHeaderSize;
  SlotHeaderWire h{};
  h.seq_begin = seq * 2 + 1;
  h.seq_end = 0;
  h.timestamp_ns = static_cast<int64_t>(timestamp_ns);
  h.monotonic_ns = steady_ns();
  h.payload_size = static_cast<uint32_t>(payload_len);
  h.width = width;
  h.height = height;
  h.format_code = spec_.format_code;
  h.flags = 0;
  std::snprintf(h.frame_id, sizeof(h.frame_id), "%s", spec_.frame_id.c_str());
  std::memcpy(data_ + slot_off, &h, sizeof(h));
  std::memcpy(data_ + payload_off, payload, payload_len);
  if (payload_len < spec_.payload_size) std::memset(data_ + payload_off + payload_len, 0, spec_.payload_size - payload_len);
  h.seq_begin = seq * 2;
  h.seq_end = seq * 2;
  h.monotonic_ns = steady_ns();
  std::memcpy(data_ + slot_off, &h, sizeof(h));
  auto* gh = reinterpret_cast<GlobalHeader*>(data_);
  gh->latest_seq = seq;
  return seq;
}

std::string ShmPublisher::registry_entry() const {
  std::ostringstream os;
  os << "{\n"
     << "      \"description\": \"" << json_escape(spec_.description) << "\",\n"
     << "      \"format_code\": " << spec_.format_code << ",\n"
     << "      \"format_name\": \"" << spec_.format_name << "\",\n"
     << "      \"frame_id\": \"" << spec_.frame_id << "\",\n"
     << "      \"header_size\": " << kHeaderSize << ",\n"
     << "      \"height\": " << spec_.height << ",\n"
     << "      \"kind\": \"" << spec_.kind_name << "\",\n"
     << "      \"payload_size\": " << spec_.payload_size << ",\n"
     << "      \"shm_name\": \"" << shm_name_ << "\",\n"
     << "      \"slot_count\": " << spec_.slot_count << ",\n"
     << "      \"slot_header_size\": " << kSlotHeaderSize << ",\n"
     << "      \"slot_stride\": " << slot_stride_ << ",\n"
     << "      \"stream_id\": \"" << spec_.stream_id << "\",\n"
     << "      \"width\": " << spec_.width << "\n"
     << "    }";
  return os.str();
}

void ShmPublisher::write_global_header(uint64_t latest_seq) {
  GlobalHeader gh{};
  std::memcpy(gh.magic, "CAPSHM1\0", 8);
  gh.version = 1;
  gh.kind = spec_.kind;
  gh.slot_count = spec_.slot_count;
  gh.slot_header_size = kSlotHeaderSize;
  gh.payload_size = spec_.payload_size;
  gh.header_size = kHeaderSize;
  gh.created_ns = wall_ns();
  gh.latest_seq = latest_seq;
  std::memcpy(data_, &gh, sizeof(gh));
}

void ShmPublisher::write_metadata() {
  const std::string meta = registry_entry();
  if (meta.size() + sizeof(uint32_t) + sizeof(GlobalHeader) > kHeaderSize) throw std::runtime_error("metadata too large");
  uint32_t len = static_cast<uint32_t>(meta.size());
  std::memcpy(data_ + sizeof(GlobalHeader), &len, sizeof(len));
  std::memcpy(data_ + sizeof(GlobalHeader) + sizeof(len), meta.data(), meta.size());
}

}  // namespace xr_capture_cpp

