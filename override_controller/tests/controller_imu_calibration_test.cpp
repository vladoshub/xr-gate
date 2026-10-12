#include <xr_override_controller/imu/controller_imu_calibration.hpp>
#include <xr_override_controller/config_io.hpp>
#include <chrono>
#include <cmath>
#include <fstream>
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
void file_uses_physical_identity_and_rejects_bad_scale() {
  const auto path=std::filesystem::temp_directory_path()/
      ("controller-imu-calibration-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+".json");
  std::ofstream(path)<<R"({"schema_version":1,"devices":{
    "gearvr_ble:left":{"accelerometer_bias_m_s2":[0.1,0.2,0.3],"accelerometer_scale":[0.99,1.01,1]},
    "gearvr_ble:right":{"accelerometer_bias_m_s2":[-0.1,-0.2,-0.3],"accelerometer_scale":[1.01,0.99,1]},
    "bad":{"accelerometer_bias_m_s2":[0,0,0],"accelerometer_scale":[1,0,1]}}})";
  const char* old=std::getenv("OVERRIDE_CONTROLLER_IMU_CALIBRATION_FILE");
  const std::string saved=old?old:"";
#ifdef _WIN32
  _putenv_s("OVERRIDE_CONTROLLER_IMU_CALIBRATION_FILE",path.string().c_str());
#else
  setenv("OVERRIDE_CONTROLLER_IMU_CALIBRATION_FILE",path.string().c_str(),1);
#endif
  const auto left=load_accelerometer_calibration("gearvr_ble:left");
  const auto right=load_accelerometer_calibration("gearvr_ble:right");
  require(left.enabled&&right.enabled,"calibration file was not read");
  near(left.bias_m_s2.y,.2f,"left received another sensor's calibration");
  near(right.bias_m_s2.y,-.2f,"right received another sensor's calibration");
  require(!load_accelerometer_calibration("unknown").enabled,"unknown physical device inherited calibration");
  require(!load_accelerometer_calibration("bad").enabled,"zero scale was accepted");
#ifdef _WIN32
  _putenv_s("OVERRIDE_CONTROLLER_IMU_CALIBRATION_FILE",saved.c_str());
#else
  if(old)setenv("OVERRIDE_CONTROLLER_IMU_CALIBRATION_FILE",saved.c_str(),1);
  else unsetenv("OVERRIDE_CONTROLLER_IMU_CALIBRATION_FILE");
#endif
  std::filesystem::remove(path);
}
int main() {
  const auto config_dir = std::filesystem::temp_directory_path() /
      ("controller-imu-profiles-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  std::filesystem::create_directories(config_dir);
  std::ofstream(config_dir / "controller_imu_calibration.json") << "{}";
  require(xr_override_controller::list_config_files(config_dir).empty(),
          "sensor calibration was mistaken for a controller bindings profile");
  std::ofstream(config_dir / "gearvr.json") << "{}";
  const auto profiles = xr_override_controller::list_config_files(config_dir);
  require(profiles.size() == 1 && profiles[0].filename() == "gearvr.json",
          "calibration made the configured controller profile ambiguous");
  std::filesystem::remove_all(config_dir);
  calibrated_force_reaches_ahrs_and_retains_real_acceleration();
  file_uses_physical_identity_and_rejects_bad_scale();
  std::cout<<"Accelerometer calibration: gravity, real acceleration, resets and device identity: OK\n";
}
