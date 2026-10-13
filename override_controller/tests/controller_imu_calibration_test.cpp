#include <xr_override_controller/imu/controller_imu_calibration.hpp>
#include <xr_override_controller/config_io.hpp>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <map>
#include <nlohmann/json.hpp>
#include <iostream>
#include <stdexcept>

using namespace xr_override_controller::imu;
constexpr float g=9.80665f;
void require(bool value,const char* message) { if(!value)throw std::runtime_error(message); }
void near(float actual,float expected,const char* message,float tolerance=0.0002f) { require(std::abs(actual-expected)<tolerance,message); }
Vec3f rotate(QuaternionXyzw q,Vec3f v) {
  Vec3f t{2*(q.y*v.z-q.z*v.y),2*(q.z*v.x-q.x*v.z),2*(q.x*v.y-q.y*v.x)};
  return {v.x+q.w*t.x+q.y*t.z-q.z*t.y,v.y+q.w*t.y+q.z*t.x-q.x*t.z,v.z+q.w*t.z+q.x*t.y-q.y*t.x};
}
RawControllerImuSample measurement(Vec3f force,const AccelerometerCalibration& c,uint64_t ns) {
  RawControllerImuSample s;s.host_timestamp_ns=ns;s.sample_interval_seconds=.01f;
  s.gyroscope_valid=s.accelerometer_valid=true;
  s.specific_force_m_s2={force.x/c.scale.x+c.bias_m_s2.x,
      force.y/c.scale.y+c.bias_m_s2.y,force.z/c.scale.z+c.bias_m_s2.z};
  return s;
}
void calibrated_force_reaches_ahrs_and_retains_real_acceleration() {
  for(const auto& c:{AccelerometerCalibration{true,{-.1f,-.08f,.43f},{.99f,.995f,.986f}},
                     AccelerometerCalibration{true,{-.02f,-.14f,.41f},{.993f,.991f,.987f}}}) {
    ControllerImuProcessor processor;
    processor.set_accelerometer_calibration(c);
    for(const auto& gravity: {Vec3f{g,0,0},Vec3f{-g,0,0},Vec3f{0,g,0},
                             Vec3f{0,-g,0},Vec3f{0,0,g},Vec3f{0,0,-g}}) {
      processor.reset();
      require(processor.accelerometer_calibrated(),"AHRS reset discarded sensor calibration");
      for(int sample=0;sample<60;++sample) {
        const auto q=processor.process(measurement(gravity,c,1'000'000'000ULL+sample*10'000'000ULL));
        require(processor.orientation_initialized(),"corrected stationary force did not initialize AHRS");
        const auto world=rotate(q,processor.corrected_sample().specific_force_m_s2);
        near(world.x,0,"stationary calibrated force drifted sideways",.01f);
        near(world.y,0,"stationary calibrated force drifted sideways",.01f);
        near(world.z,g,"stationary calibrated force invented vertical acceleration",.01f);
      }
    }
    processor.reset();
    processor.process(measurement({0,0,g},c,1'000'000'000));
    const auto q=processor.process(measurement({0,0,g+1.2f},c,1'010'000'000));
    const auto world=rotate(q,processor.corrected_sample().specific_force_m_s2);
    near(world.z-g,1.2f,"calibration normalized away real linear acceleration");
  }
}

using namespace xr_override_controller;
namespace fs = std::filesystem;
class TestProvider final : public InputProvider {
 public:
  std::map<std::string, AccelerometerCalibration> applied;
  std::map<std::string, uint32_t> capabilities;
  int changes = 0;
  int button_only_calls = 0;
  std::string provider_name() const override { return "test"; }
  std::vector<DeviceInfo> scan_devices(bool) override { return {}; }
  void flush_events(std::vector<DeviceInfo>&) override {}
  std::optional<InputEvent> wait_event(std::vector<DeviceInfo>&, int, bool) override { return {}; }
  std::string input_name(const DeviceInfo&, uint16_t, uint16_t) const override { return {}; }
  InputBindingSpec make_input_spec(const DeviceInfo&, uint16_t, uint16_t) const override { return {}; }
  xr_runtime::ControllerImuStateV1 imu_state(const DeviceInfo& device) const override {
    xr_runtime::ControllerImuStateV1 state;
    state.capability_flags = capabilities.at(device.fingerprint.uniq);
    return state;
  }
  bool set_accelerometer_calibration(const DeviceInfo& device, const AccelerometerCalibration& c) override {
    if (capabilities.at(device.fingerprint.uniq) == 0) ++button_only_calls;
    auto& previous = applied[device.fingerprint.uniq];
    if (same_accelerometer_calibration(previous, c)) return false;
    previous = c;
    ++changes;
    return true;
  }
};
void config_id_routing_roundtrip_and_capability_guard(const fs::path& directory) {
  AppConfig cfg;
  for (int id : {21, 7, 99}) {
    ConfigDevice d;
    d.id = id;
    d.fingerprint.uniq = "session-" + std::to_string(id);
    d.imu_side = id == 7 ? ControllerSide::Right : ControllerSide::Left;
    cfg.devices.push_back(d);
  }
  cfg.imu_calibrations = {
      {7, {true, {-.1f,-.2f,-.3f}, {1.01f,.99f,1}}},
      {21, {true, {.1f,.2f,.3f}, {.99f,1.01f,1}}},
      {99, {true, {.4f,.5f,.6f}, {1,1,1}}}};
  const auto path = directory / "gearvr.json";
  save_config_file(cfg, path);
  cfg = load_config_file(path);
  require(cfg.imu_calibrations.size() == 3, "saving/training discarded config calibration");
  near(cfg.imu_calibrations[0].accelerometer.bias_m_s2.y, -.2f, "config roundtrip changed coefficients");
  // Live order, config order, and calibration order deliberately disagree.
  TestProvider provider;
  std::vector<DeviceInfo> devices;
  for (int index : {1, 2, 0}) {
    DeviceInfo live;
    live.fingerprint = cfg.devices[index].fingerprint;
    live.identity_known = true;
    provider.capabilities[live.fingerprint.uniq] = index == 2 ? 0 : 3;
    devices.push_back(live);
  }
  configure_imu_calibrations(provider, cfg, devices);
  require(provider.changes == 2 && provider.button_only_calls == 0,
          "calibration applied to a button-only device or wrong number of IMUs");
  near(provider.applied["session-21"].bias_m_s2.y, .2f, "device_id=21 received id=7 calibration");
  near(provider.applied["session-7"].bias_m_s2.y, -.2f, "device_id=7 received id=21 calibration");
  for (auto& d : cfg.devices) d.imu_side = ControllerSide::Right;
  configure_imu_calibrations(provider, cfg, devices);
  require(provider.changes == 2, "changing hand routing reset/swapped accelerometer calibration");
  // --connect-devices changes a fingerprint, keeping its configured integer id.
  cfg.devices[0].fingerprint.uniq = "replacement-session";
  devices[2].fingerprint = cfg.devices[0].fingerprint;
  provider.capabilities["replacement-session"] = 3;
  configure_imu_calibrations(provider, cfg, devices);
  near(provider.applied["replacement-session"].bias_m_s2.y, .2f,
       "coefficients were still keyed by transport UUID instead of configured device_id");
  cfg.imu_calibrations.clear();
  configure_imu_calibrations(provider, cfg, devices);
  require(!provider.applied["session-7"].enabled &&
          !provider.applied["replacement-session"].enabled,
          "a profile without calibration retained previous coefficients");
  const auto changes = provider.changes;
  configure_imu_calibrations(provider, cfg, devices);
  require(provider.changes == changes, "default config repeatedly reset filters");
}
void malformed_and_ambiguous_calibration_is_rejected(const fs::path& directory) {
  const auto path = directory / "invalid.json";
  nlohmann::json valid = {{"devices", {{{"id", 1}, {"uniq", "a"}}}},
      {"imu_calibrations", {{{"device_id", 1}, {"enabled", true},
      {"accelerometer_bias_m_s2", {0,0,0}}, {"accelerometer_scale", {1,1,1}}}}}};
  auto rejects = [&](const nlohmann::json& document) {
    std::ofstream(path) << document.dump();
    bool rejected = false;
    try { load_config_file(path); } catch (const std::exception&) { rejected = true; }
    require(rejected, "malformed calibration was accepted");
  };
  auto bad = valid; bad["imu_calibrations"][0]["device_id"] = 123; rejects(bad);
  bad = valid; bad["imu_calibrations"][0]["device_id"] = 1.5; rejects(bad);
  bad = valid; bad["imu_calibrations"][0]["device_id"] = 4294967297ULL; rejects(bad);
  bad = valid; bad["imu_calibrations"][0]["accelerometer_scale"][1] = 0; rejects(bad);
  bad = valid; bad["imu_calibrations"][0]["accelerometer_bias_m_s2"][2] = 2.1; rejects(bad);
  bad = valid; bad["imu_calibrations"].push_back(bad["imu_calibrations"][0]); rejects(bad);
  bad = valid; bad["imu_calibrations"] = nlohmann::json::object(); rejects(bad);
  bad = valid; bad["imu_calibrations"][0]["accelerometer_scale"] = {1,1}; rejects(bad);
  AppConfig cfg;
  ConfigDevice d; d.id = 1; d.fingerprint.uniq = "ambiguous";
  cfg.devices.push_back(d);
  cfg.imu_calibrations.push_back({1, {true, {.1f,.2f,.3f}, {1,1,1}}});
  DeviceInfo live; live.fingerprint = d.fingerprint; live.identity_known = true;
  TestProvider provider; provider.capabilities["ambiguous"] = 3;
  configure_imu_calibrations(provider, cfg, {live, live});
  require(provider.changes == 0, "ambiguous live identity inherited sensor calibration");
  d.id = 2; cfg.devices.push_back(d);
  cfg.imu_calibrations.push_back({2, {true, {-.1f,-.2f,-.3f}, {1,1,1}}});
  bool rejected = false;
  try { configure_imu_calibrations(provider, cfg, {live}); }
  catch (const std::invalid_argument&) { rejected = true; }
  require(rejected, "two calibrated config IDs were allowed to fight over one physical IMU");
}
int main() {
  const auto directory = fs::temp_directory_path() /
      ("controller-imu-config-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  fs::create_directories(directory);
  // A legacy side file is neither a profile nor a source of corrections.
  std::ofstream(directory / "controller_imu_calibration.json") << R"({"schema_version":1,"devices":{}})";
  require(list_config_files(directory).empty(), "legacy calibration appeared in profile selection");
  const auto legacy = (directory / "controller_imu_calibration.json").string();
  const char* old = std::getenv("OVERRIDE_CONTROLLER_IMU_CALIBRATION_FILE");
  const bool had_old = old != nullptr;
  const std::string saved = old ? old : "";
#ifdef _WIN32
  _putenv_s("OVERRIDE_CONTROLLER_IMU_CALIBRATION_FILE", legacy.c_str());
#else
  setenv("OVERRIDE_CONTROLLER_IMU_CALIBRATION_FILE", legacy.c_str(), 1);
#endif
  calibrated_force_reaches_ahrs_and_retains_real_acceleration();
  config_id_routing_roundtrip_and_capability_guard(directory);
  malformed_and_ambiguous_calibration_is_rejected(directory);
#ifdef _WIN32
  _putenv_s("OVERRIDE_CONTROLLER_IMU_CALIBRATION_FILE", saved.c_str());
#else
  if (had_old) setenv("OVERRIDE_CONTROLLER_IMU_CALIBRATION_FILE", saved.c_str(), 1);
  else unsetenv("OVERRIDE_CONTROLLER_IMU_CALIBRATION_FILE");
#endif
  fs::remove_all(directory);
  std::cout << "Config accelerometer calibration: ID routing, capability, validation, gravity and resets: OK\n";
}
