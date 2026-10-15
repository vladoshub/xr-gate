#include "capture_service_cpp/tcp_fanout.hpp"

#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <array>
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <new>
#include <sstream>
#include <stdexcept>

namespace xr_capture_cpp {
std::atomic<bool> g_stop{false};
std::atomic<int> g_exit_code{kExitOk};
}
namespace {
constexpr size_t kImageBytes = 480 * 640;
std::atomic<bool> count_large_allocations{false};
std::atomic<unsigned> large_allocations{0};
void require(bool ok, const char* message) {
  if (!ok) throw std::runtime_error(message);
}
struct Socket {
  int fd = -1;
  ~Socket() { if (fd >= 0) ::close(fd); }
};
int unused_port() {
  Socket socket{::socket(AF_INET, SOCK_STREAM, 0)};
  require(socket.fd >= 0,"cannot create port probe");
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  require(::bind(socket.fd,reinterpret_cast<sockaddr*>(&addr),sizeof(addr)) == 0,"port probe bind failed");
  socklen_t len = sizeof(addr);
  require(::getsockname(socket.fd,reinterpret_cast<sockaddr*>(&addr),&len) == 0,"port probe failed");
  return ntohs(addr.sin_port);
}
void read_exact(int fd, uint8_t* data, size_t size) {
  while (size) {
    const ssize_t got = ::recv(fd,data,size,0);
    require(got > 0,"TCP test read timed out or connection closed");
    data += got;
    size -= static_cast<size_t>(got);
  }
}
std::string read_line(int fd) {
  std::string result;
  while (result.size() < 4096) {
    uint8_t c;
    read_exact(fd,&c,1);
    if (c == '\n') return result;
    result += static_cast<char>(c);
  }
  throw std::runtime_error("TCP test line too long");
}
std::string read_text(int fd, size_t count) {
  std::string result(count,'\0');
  read_exact(fd,reinterpret_cast<uint8_t*>(result.data()),count);
  return result;
}
bool readable(int fd, int timeout_ms) {
  pollfd p{fd,POLLIN,0};
  return ::poll(&p,1,timeout_ms) > 0 && (p.revents & POLLIN);
}
struct Packet { std::string header; std::vector<uint8_t> payload; };
Packet read_packet(int fd) {
  std::istringstream line(read_line(fd));
  std::string prefix;
  size_t header_size=0,payload_size=0;
  line >> prefix >> header_size >> payload_size;
  require(prefix == "CAPMSG1" && header_size < 16384 && payload_size <= kImageBytes,"invalid CAPMSG1 frame");
  Packet packet{read_text(fd,header_size),std::vector<uint8_t>(payload_size)};
  read_exact(fd,packet.payload.data(),packet.payload.size());
  return packet;
}
}

void* operator new(std::size_t size) {
  if (count_large_allocations.load() && size >= kImageBytes) ++large_allocations;
  if (void* p=std::malloc(size ? size : 1)) return p;
  throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p,std::size_t) noexcept { std::free(p); }

int main() {
  using namespace xr_capture_cpp;
  StreamSpec camera{"camera0",kKindImage,"IMAGE",480,640,kFormatGray8,"GRAY8",kImageBytes,8,"camera0","test"};
  StreamSpec imu{"imu0",kKindImu,"IMU",0,0,kFormatImuF32Le,"IMU_F32_LE",24,16,"imu0","test"};
  const int port=unused_port();
  TcpFanoutPublisher publisher({{camera.stream_id,camera},{imu.stream_id,imu}},"127.0.0.1",port,"test","test",32);
  std::vector<uint8_t> image(kImageBytes,71);
  auto publish_image=[&] {
    publisher.publish("camera0",image.data(),image.size(),1,2,480,640,kFormatGray8,0,"camera0");
  };
  count_large_allocations=true;
  for(int i=0;i<100;++i) publish_image();
  count_large_allocations=false;
  require(large_allocations == 0,"TCP copied camera images without clients");

  publisher.start();
  Socket client{::socket(AF_INET,SOCK_STREAM,0)};
  timeval timeout{2,0};
  ::setsockopt(client.fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));
  sockaddr_in addr{};
  addr.sin_family=AF_INET; addr.sin_addr.s_addr=htonl(INADDR_LOOPBACK); addr.sin_port=htons(port);
  require(::connect(client.fd,reinterpret_cast<sockaddr*>(&addr),sizeof(addr)) == 0,"TCP test connect failed");
  std::istringstream hello_line(read_line(client.fd));
  std::string prefix;
  size_t hello_size=0;
  hello_line >> prefix >> hello_size;
  require(prefix == "CAPHELLO" && hello_size < 16384,"invalid CAPHELLO");
  const std::string hello=read_text(client.fd,hello_size);
  require(hello.find("camera0") != std::string::npos && hello.find("imu0") != std::string::npos,"hello lost stream metadata");
  const std::string subscribe="SUBSCRIBE imu0\n";
  require(::send(client.fd,subscribe.data(),subscribe.size(),0) == static_cast<ssize_t>(subscribe.size()),"subscription write failed");
  std::array<uint8_t,24> sample{};
  for(size_t i=0;i<sample.size();++i) sample[i]=static_cast<uint8_t>(i);
  auto publish_imu=[&](uint64_t sequence) {
    publisher.publish("imu0",sample.data(),sample.size(),sequence,456,0,0,kFormatImuF32Le,0,"imu0");
  };
  bool ready=false;
  for(int i=0;i<100 && !ready;++i) {
    publish_imu(17);
    ready=readable(client.fd,10);
  }
  require(ready,"subscriber did not receive IMU");
  require(read_packet(client.fd).payload == std::vector<uint8_t>(sample.begin(),sample.end()),"IMU payload changed");
  large_allocations=0;
  count_large_allocations=true;
  for(int i=0;i<100;++i) publish_image();
  count_large_allocations=false;
  require(large_allocations == 0,"IMU-only subscriber caused camera payload copies");
  publish_imu(9876543);
  bool marker=false;
  for(int i=0;i<101 && !marker;++i) {
    Packet packet=read_packet(client.fd);
    require(packet.header.find("\"stream_id\":\"imu0\"") != std::string::npos,"filtered camera frame leaked to IMU client");
    require(packet.payload == std::vector<uint8_t>(sample.begin(),sample.end()),"wire payload changed");
    marker=packet.header.find("\"sequence\":9876543") != std::string::npos;
  }
  require(marker,"wire sequence changed or delivery stalled");

  Socket all_streams{::socket(AF_INET,SOCK_STREAM,0)};
  ::setsockopt(all_streams.fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));
  require(::connect(all_streams.fd,reinterpret_cast<sockaddr*>(&addr),sizeof(addr)) == 0,"wildcard client connect failed");
  std::istringstream all_hello_line(read_line(all_streams.fd));
  all_hello_line >> prefix >> hello_size;
  require(prefix == "CAPHELLO" && hello_size < 16384,"invalid wildcard CAPHELLO");
  (void)read_text(all_streams.fd,hello_size);
  const std::string subscribe_all="SUBSCRIBE *\n";
  require(::send(all_streams.fd,subscribe_all.data(),subscribe_all.size(),0) == static_cast<ssize_t>(subscribe_all.size()),"wildcard subscription write failed");
  ready=false;
  for(int i=0;i<100 && !ready;++i) {
    publish_image();
    ready=readable(all_streams.fd,10);
  }
  require(ready,"wildcard subscriber did not receive camera frames");
  const Packet frame=read_packet(all_streams.fd);
  require(frame.header.find("\"stream_id\":\"camera0\"") != std::string::npos,"camera stream header changed");
  require(frame.payload == image,"camera payload changed");
  require(!readable(client.fd,10),"camera frame leaked to the IMU-only subscriber");
  publisher.stop();
  std::cout << "TCP demand filtering, no unused image copies, CAPHELLO/CAPMSG1 and payload delivery: OK\n";
}
