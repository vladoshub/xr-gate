#pragma once

#include <array>
#include <cstdint>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>
#include <xr_runtime/contracts/runtime_adapter.hpp>
#include <xr_runtime/contracts/runtime_controller_state_contract.hpp>

namespace xr_runtime_adapter::emulation {
using Vec3 = std::array<double, 3>;
using Basis = std::array<Vec3, 3>;
using ThumbPose = std::array<double, 12>;
// Clockwise neighbours in stick coordinates, starting at right.
inline constexpr std::array<const char*, 8> thumb_direction_names{{
    "right", "up_right", "up", "up_left", "left", "down_left", "down", "down_right"}};
inline constexpr std::array<Vec3, 8> thumb_direction_axes{{
    {1, 0, 0}, {1, 1, 0}, {0, 1, 0}, {-1, 1, 0},
    {-1, 0, 0}, {-1, -1, 0}, {0, -1, 0}, {1, -1, 0}}};
struct HandMeasurement {
  bool palm_valid = false, thumb_valid = false;
  Basis palm_basis{};
  double thumb_clearance = 0, thumb_straightness = 0, pinch_ratio = -1;
  bool thumb_pad_valid = false;
  Vec3 thumb_feature{};
  ThumbPose thumb_pose{};
  double thumb_pad_clearance = 0;
  double index_chain_curl = 1, index_knuckle_curl = 1;
  std::array<double, 4> curl{{-1, -1, -1, -1}};
};
struct ThumbCalibration {
  bool calibrated = false;
  bool skeleton = false;
  std::vector<ThumbPose> neutral_poses;
  std::array<ThumbPose, 8> pose_directions{};  // absolute palm-local poses
  Vec3 neutral{}, axis_x{}, axis_y{};
  std::array<Vec3, 4> directions{};
  double deadzone = .12, enable_clearance = .7, disable_clearance = .6;
};
struct HandConfig {
  bool enabled = true, calibrated = false;
  bool invert_x = false, invert_y = false;
  Vec3 neutral{}, axis_x{}, axis_y{};
  bool directional_calibration = false;
  std::array<Vec3, 4> directions{};  // left, right, forward, back, relative to neutral
  double deadzone = -1;
  std::array<double, 4> open_curl{{0, 0, 0, 0}}, closed_curl{{1, 1, 1, 1}};
  bool grip_calibrated = false;
  double grip_open_curl = 0, grip_closed_curl = 1;
  std::string grip_calibration_finger = "little";
  bool trigger_calibrated = false;
  double trigger_open_curl = 0, trigger_closed_curl = 1;
  bool click_calibrated = false;
  double click_open_curl = 0, click_closed_curl = 1;
  ThumbCalibration thumb;
};
struct Config {
  HandConfig left, right;
  std::string dpad_mode = "four_way";
  std::string dpad_source = "wrist", thumb_dpad_mode = "eight_way";
  std::string button_layout = "legacy";
  std::string thumbstick_click_gesture = "layout_default";
  std::string profile_path;
  bool wrist_filter_enabled = true;
  bool trigger_on_curl = false, grip_on_curl = false;
  bool thumb_invert_y = false;
  bool thumbstick_click_on_curl = false;
  double wrist_filter_degrees = 2;
  // Final presentation yaw about runtime +Y; positive turns both hands outward.
  double pose_yaw_offset_degrees = 0;
  double range_degrees = 30, deadzone = 0.18, smoothing_ms = 40;
  double forward_range_scale = 1;
  double thumb_back_on = .85, thumb_back_off = .70;
  double min_confidence = 0.3, max_age_ms = 120;
  double extension_on = 0.7, extension_off = 0.45;
  double little_extension_on = 0.60, little_extension_off = 0.45;
  double curl_on = 0.65, curl_off = 0.45;
  double button_confirm_ms = 45, pose_confirm_ms = 120, menu_hold_ms = 350;
  double pinch_on_ratio = 0.30, pinch_off_ratio = 0.50;
  double thumb_enable_clearance = 0.85, thumb_disable_clearance = 0.80;
  double thumb_extension_on = 0.86, thumb_extension_off = 0.80;
};
Config config_from_json(const nlohmann::json& json);
Config load_config(const std::string& path);
HandMeasurement measure_hand(const xr_runtime::HandSideF32V2& hand, double min_confidence);
// World up expressed in the palm basis. Independent of world yaw and thumb direction.
Vec3 wrist_feature(const HandMeasurement& hand, const Basis& reference);
void calibrate_axes(HandConfig& config, const Vec3& neutral, const Vec3& left, const Vec3& right,
                    const Vec3& up, const Vec3& down);
Vec3 project_wrist(const HandConfig& config, const Vec3& feature);
Vec3 project_thumb(const ThumbCalibration& config, const Vec3& feature);
Vec3 project_thumb(const ThumbCalibration& config, const HandMeasurement& hand);
void validate_thumb_skeleton(const ThumbCalibration& config);
class Controller {
 public:
  explicit Controller(Config config);
  void apply(xr_runtime::RuntimeControllerStateFrameV1& output,
             const xr_runtime::HandTrackingFrameF32V2* hands, uint64_t now_ns,
             uint64_t origin_reset_counter = 0, bool gestures_blocked = false);

 private:
  enum class Finger { Unknown, Curled, Extended };
  enum class Pose { None, Pinch, A, B, PadClick };
  struct Channel {
    bool active = false, candidate = false;
    uint64_t since_ns = 0;
  };
  struct SideState {
    bool valid = false, fresh = false, reset_event = false, have_sample = false;
    bool anchored = false, thumb_active = false, pinch_active = false, wait_for_neutral = false;
    bool menu_open = false;
    bool click_neutral_required = false;
    bool click_armed = false;
    uint64_t click_neutral_since_ns = 0;
    bool thumb_back_active = false;
    uint64_t sequence = 0, sample_ns = 0, reset_counter = 0, origin_counter = 0;
    Basis reference{};
    Vec3 neutral{}, axis_x{}, axis_y{};
    std::array<Finger, 4> fingers{};
    std::array<double, 4> extension{};
    bool grip_finger_active = false;
    double grip_extension = 0;
    bool trigger_finger_active = false, trigger_released = false;
    double trigger_extension = 0;
    bool click_finger_active = false, click_released = false;
    double click_extension = 0;
    Channel trigger_channel{}, grip_channel{}, click_channel{};
    Pose candidate = Pose::None, active = Pose::None;
    uint64_t pose_since_ns = 0;
    double x = 0, y = 0;
    uint64_t buttons = 0, touches = 0, dpad = 0, previous_buttons = 0;
    bool physical_present = false;
    uint64_t physical_device_hash = 0, previous_emulated_buttons = 0;
    float trigger = 0, grip = 0;
    std::array<uint32_t, 32> presses{}, releases{};
    std::array<uint32_t, 32> physical_presses{}, physical_releases{};
  };
  void update_side(const xr_runtime::HandSideF32V2* hand, const HandConfig& config,
                   SideState& state, const xr_runtime::HandTrackingFrameF32V2* frame,
                   uint64_t now_ns, uint64_t origin_reset_counter);
  void update_buttons(const HandMeasurement& hand, const HandConfig& config,
                      SideState& state, uint64_t sample_ns);
  void update_pad(const HandMeasurement& hand, const HandConfig& config, SideState& state,
                  double dt_ms, bool blocked);
  void update_menu();
  void publish_side(xr_runtime::RuntimeControllerSideStateV1& output, const HandConfig& config,
                    SideState& state);
  static void clear_input(SideState& state);
  bool is_released_pose(const SideState& state) const;
  Config config_;
  SideState left_, right_;
  bool menu_candidate_ = false, menu_active_ = false;
  uint64_t menu_since_ns_ = 0, menu_sample_ns_ = 0;
};
}  // namespace xr_runtime_adapter::emulation
