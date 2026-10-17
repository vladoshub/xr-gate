#include <capture_client/transports/capture_service_tcp_transport.hpp>
#include <capture_client/sync/synchronizer.hpp>
#include <xr_video/recording/stereo_video_recorder.hpp>
#include <basalt_input_feeder.hpp>
#include <tbb/concurrent_queue.h>
#include <cassert>
#include <future>
#include <iostream>
#include <map>

using namespace capture_client;
struct Reader : IStreamReader {
  StreamInfo meta;
  uint64_t latest = 0;
  std::map<uint64_t, ImageFrame> images;
  std::map<uint64_t, ImuSample> samples;
  const StreamInfo& info() const override { return meta; }
  uint64_t latest_sequence() const override { return latest; }
  bool read_latest(RawMessage&) const override { return false; }
  bool read_sequence(uint64_t, RawMessage&) const override { return false; }
  bool read_image_sequence(uint64_t seq, ImageFrame& out) const override {
    auto it = images.find(seq); if (it == images.end()) return false;
    out = it->second; return true;
  }
  bool read_imu_sequence(uint64_t seq, ImuSample& out) const override {
    auto it = samples.find(seq); if (it == samples.end()) return false;
    out = it->second; return true;
  }
  bool read_latest_imu(ImuSample& out) const override { return read_imu_sequence(latest, out); }
  void image(uint64_t seq, int64_t ts) {
    ImageFrame f; f.sequence=seq; f.timestamp_ns=ts; f.width=2; f.height=2; f.gray8={1,2,3,4};
    images[seq]=f; latest=seq;
  }
  void imu(uint64_t seq, int64_t ts) {
    ImuSample s; s.sequence=seq; s.timestamp_ns=ts; s.accel_m_s2[2]=9.81;
    samples[seq]=s; latest=seq;
  }
};
struct Transport : ICaptureTransport {
  Reader a,b,c;
  const std::string& type() const override { static std::string t="test"; return t; }
  IStreamReader& cam0() override { return a; }
  IStreamReader& cam1() override { return b; }
  IStreamReader& imu() override { return c; }
};
template<class Fn> void must_throw(Fn fn) {
  bool threw=false; try { fn(); } catch (const std::exception&) { threw=true; }
  assert(threw);
}

void tcp_timeout(bool legacy) {
  int listener=::socket(AF_INET,SOCK_STREAM,0); assert(listener>=0);
  sockaddr_in addr{}; addr.sin_family=AF_INET; addr.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
  assert(::bind(listener,reinterpret_cast<sockaddr*>(&addr),sizeof(addr))==0);
  socklen_t len=sizeof(addr);
  assert(::getsockname(listener,reinterpret_cast<sockaddr*>(&addr),&len)==0);
  assert(::listen(listener,1)==0);
  std::thread server([&] {
    int fd=::accept(listener,nullptr,nullptr); assert(fd>=0);
    if (!legacy) {
      std::string hello=R"({"streams":{"camera0":{},"camera1":{}}})";
      std::string wire="CAPHELLO "+std::to_string(hello.size())+"\n"+hello;
      capture_service_tcp_detail::write_all(fd,wire);
    }
    char buffer[128];
    while (::recv(fd,buffer,sizeof(buffer),0)>0) {}
    ::close(fd);
  });
  must_throw([&] {
    if (legacy) {
      TcpCaptureTransportConfig cfg; cfg.port=ntohs(addr.sin_port);
      cfg.subscribe_imu=false; cfg.first_packet_timeout_s=.03;
      TcpCaptureTransport transport(cfg);
    } else {
      CaptureServiceTcpTransportConfig cfg; cfg.port=ntohs(addr.sin_port);
      cfg.subscribe_imu=false; cfg.first_packet_timeout_s=.03;
      CaptureServiceTcpTransport transport(cfg);
    }
  });
  server.join(); ::close(listener);
}

void imu_continuity() {
  for (int failure=0; failure<5; ++failure) {
    Transport t;
    StereoImuSynchronizer sync(t,1'000'000,0.,true,-1'000'000,true,true);
    t.a.image(1,11'000'000); t.b.image(1,11'000'000);
    t.c.imu(1,9'000'000); t.c.imu(2,11'000'000);
    auto first=sync.read_next(.01);
    assert(first && first->pair.timestamp_ns==10'000'000 && first->imu_samples.size()==2);
    t.a.image(2,41'000'000); t.b.image(2,41'000'000);
    if (failure==0) { t.c.samples.clear(); t.c.imu(20,40'000'000); t.c.imu(21,41'000'000); }
    if (failure==1) { /* IMU stopped. */ }
    if (failure==2) { t.c.imu(3,10'000'000); }
    if (failure==3) { t.c.imu(3,80'000'000); }
    if (failure==4) { t.c.samples.clear(); t.c.imu(1,41'000'000); }
    must_throw([&] { sync.read_next(.01); });
  }
  Transport t;
  StereoImuSynchronizer vo(t,1'000'000,0.,false);
  t.a.image(1,10); t.b.image(1,10);
  assert(vo.read_next(.01));

  Transport healthy;
  StereoImuSynchronizer sync(healthy,1'000'000,0.,true,-1'000'000,true,true);
  healthy.a.image(1,11'000'000); healthy.b.image(1,11'000'000);
  healthy.c.imu(1,9'000'000); healthy.c.imu(2,11'000'000);
  assert(sync.read_next(.01));
  healthy.a.image(2,41'000'000); healthy.b.image(2,41'000'000);
  for (uint64_t seq=3; seq<=32; ++seq) healthy.c.imu(seq,(seq+9)*1'000'000);
  auto second=sync.read_next(.01);
  assert(second && second->pair.timestamp_ns==40'000'000);
  assert(second->previous_camera_timestamp_ns==10'000'000);
  assert(second->imu_samples.front().sequence==3 && second->imu_samples.back().sequence==32);
  healthy.a.image(3,40'000'000); healthy.b.image(3,40'000'000);
  must_throw([&]{sync.read_next(.01);});
}

void independent_imu_delivery() {
  using Ptr=std::shared_ptr<int>;
  tbb::concurrent_bounded_queue<Ptr> imu,vision;
  imu.set_capacity(300); vision.set_capacity(10);
  std::atomic<bool> finished{false};
  xr_basalt::InputFeeder<decltype(imu),Ptr> feeder(imu,[&]{return finished.load();});
  auto worker=std::async(std::launch::async,[&]{
    Ptr value;
    imu.pop(value); assert(value && *value==1);
    vision.pop(value); assert(value && *value==1);
    for (int seq=2; seq<=501; ++seq) { imu.pop(value); assert(value && *value==seq); }
    imu.pop(value); assert(!value);
    finished=true;
  });
  std::vector<Ptr> batch;
  for (int seq=1; seq<=501; ++seq) batch.push_back(std::make_shared<int>(seq));
  feeder.submit(std::move(batch));
  vision.push(std::make_shared<int>(1));
  feeder.finish();
  assert(worker.wait_for(std::chrono::seconds(2))==std::future_status::ready);
  worker.get(); feeder.join();

  // A stopped estimator must not leave the feeder blocked on a full queue.
  xr_basalt::InputFeeder<decltype(imu),Ptr> stopped(imu,[&]{return finished.load();});
  stopped.submit(std::vector<Ptr>(501,std::make_shared<int>(2)));
  stopped.finish(); stopped.join();
}

xr_video::StereoVideoFrame frame(uint8_t value) {
  xr_video::StereoVideoFrame f;
  f.header.width=2; f.header.height=2; f.header.stride_left=2; f.header.stride_right=2;
  f.header.left_size_bytes=4; f.header.right_size_bytes=4;
  f.header.sequence=1; f.header.source_timestamp_ns=1;
  f.left=std::vector<uint8_t>(4,value); f.right=f.left;
  return f;
}
void recordings(const std::filesystem::path& root) {
  setenv("TMPDIR",root.c_str(),1);
  xr_video::StereoVideoRecorder rec;
  rec.start({}); auto a=rec.status().record_dir; rec.record(frame(7)); rec.stop();
  rec.start({}); auto b=rec.status().record_dir; rec.record(frame(9)); rec.stop();
  assert(a!=b);
  std::ifstream pixels(std::filesystem::path(a)/"cam0/00000001.pgm",std::ios::binary);
  std::string bytes((std::istreambuf_iterator<char>(pixels)),{}); assert(bytes.back()==7);
  xr_video::StereoVideoRecorderConfig cfg; cfg.record_dir=a;
  must_throw([&]{rec.start(cfg);});
  cfg.record_dir=(root/"full").string();
  std::filesystem::create_directories(std::filesystem::path(cfg.record_dir)/"cam0");
  std::filesystem::create_symlink("/dev/full",std::filesystem::path(cfg.record_dir)/"cam0/00000001.pgm");
  rec.start(cfg);
  must_throw([&]{rec.record(frame(1));});
  assert(!rec.status().active && rec.status().frames_written==0 && !rec.status().error.empty());
  rec.stop();
  std::ifstream metadata(std::filesystem::path(cfg.record_dir)/"metadata.json");
  nlohmann::json j; metadata>>j; assert(j["finalized"]==false && j["frames_written"]==0);
  rec.start({}); rec.record(frame(2));
  auto meta=std::filesystem::path(rec.status().record_dir)/"metadata.json";
  std::filesystem::create_symlink("/dev/full",meta.string()+".tmp");
  must_throw([&]{rec.stop();});
  assert(!rec.status().error.empty());
  std::ifstream previous(meta); previous>>j; assert(j["finalized"]==false);
}

int main() {
  tcp_timeout(false); tcp_timeout(true);
  imu_continuity(); independent_imu_delivery();
  char pattern[]="/tmp/xr-backend-recovery-XXXXXX";
  const char* dir=::mkdtemp(pattern); assert(dir);
  recordings(dir); std::filesystem::remove_all(dir);
  std::cout << "TCP timeout cleanup, IMU continuity, queue delivery and recorder failures passed\n";
}
