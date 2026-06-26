#include "capture_service_cpp/shm_publisher.hpp"

#include <stdexcept>

namespace xr_capture_cpp {

ShmPublisher::ShmPublisher(const StreamSpec& spec, const std::string& namespace_name) : spec_(spec) {
  (void)namespace_name;
  throw std::runtime_error("POSIX SHM publisher is not supported on native Windows; use --publish tcp");
}
ShmPublisher::~ShmPublisher() = default;
uint64_t ShmPublisher::publish(const uint8_t*, size_t, uint64_t, uint32_t, uint32_t) { return 0; }
std::string ShmPublisher::registry_entry() const { return "{}"; }
void ShmPublisher::write_global_header(uint64_t) {}
void ShmPublisher::write_metadata() {}

}  // namespace xr_capture_cpp
