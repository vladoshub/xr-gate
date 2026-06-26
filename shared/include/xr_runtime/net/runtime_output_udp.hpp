#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>

#include <xr_runtime/contracts/runtime_pose_stream.hpp>
#include <xr_runtime/contracts/runtime_controller_state_contract.hpp>
#include <xr_runtime/platform/udp_socket.hpp>

namespace xr_runtime::net::runtime_output_udp {

#pragma pack(push, 1)
struct PacketHeader {
  char magic[4];          // "XRRO" == XR runtime output
  uint16_t version;       // 1
  uint16_t header_size;   // sizeof(PacketHeader)
  uint16_t packet_type;   // PacketType
  uint16_t flags;         // reserved
  uint32_t payload_size;

  uint64_t transport_sequence;
  uint64_t stream_sequence;
  uint64_t send_timestamp_ns;
  uint64_t source_timestamp_ns;
};
#pragma pack(pop)

static_assert(sizeof(PacketHeader) == 48, "runtime output UDP header must stay ABI-stable");

constexpr char MAGIC[4] = {'X', 'R', 'R', 'O'};
constexpr uint16_t VERSION = 1;

enum class PacketType : uint16_t {
  RuntimeHmdPose = 1,
  RuntimeHandTracking = 2,
  RuntimeControllerState = 3,
};

template <typename PayloadT>
struct PacketBytes {
  std::array<uint8_t, sizeof(PacketHeader) + sizeof(PayloadT)> bytes{};
  const uint8_t* data() const { return bytes.data(); }
  uint8_t* data() { return bytes.data(); }
  size_t size() const { return bytes.size(); }
};

inline int64_t now_ns() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

inline PacketHeader make_header(PacketType type,
                                uint64_t transport_sequence,
                                uint64_t stream_sequence,
                                uint64_t send_timestamp_ns,
                                uint64_t source_timestamp_ns,
                                uint32_t payload_size) {
  PacketHeader h{};
  std::memcpy(h.magic, MAGIC, sizeof(MAGIC));
  h.version = VERSION;
  h.header_size = static_cast<uint16_t>(sizeof(PacketHeader));
  h.packet_type = static_cast<uint16_t>(type);
  h.flags = 0;
  h.payload_size = payload_size;
  h.transport_sequence = transport_sequence;
  h.stream_sequence = stream_sequence;
  h.send_timestamp_ns = send_timestamp_ns;
  h.source_timestamp_ns = source_timestamp_ns;
  return h;
}

inline bool magic_ok(const PacketHeader& h) {
  return std::memcmp(h.magic, MAGIC, sizeof(MAGIC)) == 0;
}

inline PacketHeader decode_header(const void* data, size_t size) {
  if (size < sizeof(PacketHeader)) throw std::runtime_error("runtime output UDP packet is shorter than header");
  PacketHeader h{};
  std::memcpy(&h, data, sizeof(h));
  if (!magic_ok(h)) throw std::runtime_error("runtime output UDP magic mismatch");
  if (h.version != VERSION) throw std::runtime_error("runtime output UDP version mismatch");
  if (h.header_size != sizeof(PacketHeader)) throw std::runtime_error("runtime output UDP header size mismatch");
  if (size < sizeof(PacketHeader) + h.payload_size) throw std::runtime_error("runtime output UDP packet is shorter than payload");
  return h;
}

template <typename PayloadT>
PacketBytes<PayloadT> encode_packet(PacketType type,
                                    uint64_t transport_sequence,
                                    uint64_t stream_sequence,
                                    uint64_t send_timestamp_ns,
                                    uint64_t source_timestamp_ns,
                                    const PayloadT& payload) {
  PacketBytes<PayloadT> out{};
  const PacketHeader h = make_header(type,
                                     transport_sequence,
                                     stream_sequence,
                                     send_timestamp_ns,
                                     source_timestamp_ns,
                                     static_cast<uint32_t>(sizeof(PayloadT)));
  std::memcpy(out.data(), &h, sizeof(h));
  std::memcpy(out.data() + sizeof(h), &payload, sizeof(PayloadT));
  return out;
}

template <typename PayloadT>
bool decode_payload(const PacketHeader& h,
                    const void* packet_data,
                    size_t packet_size,
                    PacketType expected_type,
                    PayloadT& out) {
  if (h.packet_type != static_cast<uint16_t>(expected_type)) return false;
  if (h.payload_size != sizeof(PayloadT)) return false;
  if (packet_size < sizeof(PacketHeader) + sizeof(PayloadT)) return false;
  std::memcpy(&out, static_cast<const uint8_t*>(packet_data) + sizeof(PacketHeader), sizeof(PayloadT));
  return true;
}

class RuntimeOutputUdpSender {
 public:
  RuntimeOutputUdpSender(std::string host, uint16_t port)
      : sender_(std::move(host), port) {}

  void send_pose(const RuntimeHmdPoseF64V1& pose) {
    const auto packet = encode_packet(PacketType::RuntimeHmdPose,
                                      ++transport_sequence_,
                                      pose.sequence,
                                      static_cast<uint64_t>(now_ns()),
                                      pose.timestamp_ns != 0 ? pose.timestamp_ns : pose.target_timestamp_ns,
                                      pose);
    sender_.send_packet(packet.data(), packet.size());
  }

  template <typename HandFrameT>
  void send_hand(const HandFrameT& frame) {
    const auto packet = encode_packet(PacketType::RuntimeHandTracking,
                                      ++transport_sequence_,
                                      frame.sequence,
                                      static_cast<uint64_t>(now_ns()),
                                      frame.timestamp_ns,
                                      frame);
    sender_.send_packet(packet.data(), packet.size());
  }

  void send_controller_state(const RuntimeControllerStateFrameV1& frame) {
    const auto packet = encode_packet(PacketType::RuntimeControllerState,
                                      ++transport_sequence_,
                                      frame.sequence,
                                      static_cast<uint64_t>(now_ns()),
                                      frame.timestamp_ns,
                                      frame);
    sender_.send_packet(packet.data(), packet.size());
  }

 private:
  xr_runtime::platform::UdpSender sender_;
  uint64_t transport_sequence_ = 0;
};

}  // namespace xr_runtime::net::runtime_output_udp
