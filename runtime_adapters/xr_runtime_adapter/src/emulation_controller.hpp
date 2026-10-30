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
using RingPose = std::array<double, 9>;
// MCP, PIP and DIP flexion of index/middle, in quarter turns. Joint angles
// are invariant under rigid wrist rotation, translation, hand size and mirror.
using FingerPairPose = std::array<double, 6>;
struct FingerPairCalibration {
  bool calibrated = false;
  std::array<std::vector<FingerPairPose>, 4> poses;  // open, index, middle, both
  double max_distance = 1;
};
// Clockwise neighbours in stick coordinates, starting at right.
inline constexpr std::array<const char*, 8> thumb_direction_names{{
    "right", "up_right", "up", "up_left", "left", "down_left", "down", "down_right"}};
inline constexpr std::array<Vec3, 8> thumb_direction_axes{{
    {1, 0, 0}, {1, 1, 0}, {0, 1, 0}, {-1, 1, 0},
    {-1, 0, 0}, {-1, -1, 0}, {0, -1, 0}, {1, -1, 0}}};
struct HandMeasurement {
  bool palm_valid = false, thumb_valid = false;
  Basis palm_basis{};
  Vec3 palm_center{};
  double thumb_clearance = 0, thumb_straightness = 0, pinch_ratio = -1;
  bool thumb_pad_valid = false;
  Vec3 thumb_feature{};
  ThumbPose thumb_pose{};
  double thumb_pad_clearance = 0;
  double index_chain_curl = 1, index_knuckle_curl = 1;
  bool ring_angles_valid = false;
  RingPose ring_pose{};
  Vec3 ring_angles_degrees{};  // MCP, PIP, DIP; independent of the saturated curl value
  bool finger_pair_valid = false;
  FingerPairPose finger_pair_pose{};
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
struct RingClickPoseConfig {
  bool calibrated = false;
  std::vector<RingPose> poses;
  double max_distance = .10, release_distance = .15;
};
struct ThumbBackPoseConfig {
  bool calibrated = false, reference_invert_y = false;
  std::vector<ThumbPose> poses, neutral_poses;
  double neutral_distance = .08;
  double max_distance = .09, release_distance = .115;
  double ip_max_degrees = -20, ip_release_degrees = -15;
};
struct ThumbActivityConfig {
  bool calibrated = false;
  std::vector<ThumbPose> tucked, neutral;
  double max_distance = .085, release_distance = .105;
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
  ThumbBackPoseConfig thumb_back_pose;
  ThumbActivityConfig thumb_activity;
  RingClickPoseConfig ring_click_pose;
  FingerPairCalibration finger_pair;
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
  double thumb_back_side_ratio = 0;
  double min_confidence = 0.3, max_age_ms = 120;
  double extension_on = 0.7, extension_off = 0.45;
  double little_extension_on = 0.60, little_extension_off = 0.45;
  double curl_on = 0.65, curl_off = 0.45;
  // -1 retains the selected layout/polarity's original release threshold.
  double grip_release_threshold = -1;
  // Folded MCP with unfolded phalanges: the user's half-extended ring pose.
  double ring_click_mcp_min_degrees = 65, ring_click_mcp_max_degrees = 120;
  double ring_click_pip_max_degrees = 45, ring_click_dip_max_degrees = 45;
  double ring_click_hysteresis_degrees = 10;
  double button_confirm_ms = 45, pose_confirm_ms = 120, menu_hold_ms = 350;
  // Positive palm normal points toward the viewer in this runtime convention.
  double palm_facing_on = .75, palm_facing_off = .60;
  double pinch_on_ratio = 0.30, pinch_off_ratio = 0.50;
  double thumb_enable_clearance = 0.85, thumb_disable_clearance = 0.80;
  double thumb_extension_on = 0.86, thumb_extension_off = 0.80;
};
Config config_from_json(const nlohmann::json& json);
Config load_config(const std::string& path);
HandMeasurement measure_hand(const xr_runtime::HandSideF32V2& hand, double min_confidence);
struct OpenPalmPose {
  bool opened = false, toward_viewer = false, away_from_viewer = false;
};
// Shared by the emulated touchpad click and SIMPLE_GESTURES Grip.
OpenPalmPose measure_open_palm_pose(const HandMeasurement& hand, bool fingers_extended,
                                   const Config& config, const Vec3* viewer_position,
                                   bool was_toward_viewer, bool was_away_from_viewer);
// Anatomical curl strengths; {-1,-1} means invalid or outside the sample range.
std::array<double, 2> project_finger_pair(const FingerPairCalibration& config,
                                       const HandMeasurement& hand);
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
             uint64_t origin_reset_counter = 0, bool gestures_blocked = false,
             const Vec3* viewer_position = nullptr);

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
    bool palm_click = false, thumb_tucked = false, thumb_neutral = false;
    bool click_neutral_required = false;
    bool click_armed = false;
    bool ring_click_armed = false;
    uint64_t click_neutral_since_ns = 0;
    bool thumb_back_active = false, thumb_back_pose_active = false;
    uint64_t sequence = 0, sample_ns = 0, reset_counter = 0, origin_counter = 0;
    Basis reference{};
    Vec3 neutral{}, axis_x{}, axis_y{};
    std::array<Finger, 4> fingers{};
    std::array<double, 4> extension{};
    bool grip_finger_active = false;
    bool grip_released = false;
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
                    SideState& state, bool left);
  static void clear_input(SideState& state);
  bool is_released_pose(const SideState& state) const;
  Config config_;
  bool viewer_valid_ = false;
  Vec3 viewer_position_{};
  SideState left_, right_;
  bool menu_candidate_ = false, menu_active_ = false;
  uint64_t menu_since_ns_ = 0, menu_sample_ns_ = 0;
};
}  // namespace xr_runtime_adapter::emulation
