// Exercise the real driver callbacks against the pinned Monado headers. Only
// allocation/debug/display helpers and incoming samples are replaced here.
#include "../src/xr_monado_driver.cpp"

#include <cassert>
#include <iostream>
#include <limits>
#include <thread>

extern "C" {
void* u_device_allocate(u_device_alloc_flags flags, size_t size, size_t inputs, size_t outputs) {
  auto* device = static_cast<xrt_device*>(std::calloc(1, size));
  device->inputs = static_cast<xrt_input*>(std::calloc(inputs, sizeof(xrt_input)));
  device->input_count = static_cast<uint32_t>(inputs);
  device->outputs = static_cast<xrt_output*>(std::calloc(outputs, sizeof(xrt_output)));
  device->output_count = static_cast<uint32_t>(outputs);
  device->tracking_origin = static_cast<xrt_tracking_origin*>(std::calloc(1, sizeof(xrt_tracking_origin)));
  if ((flags & U_DEVICE_ALLOC_HMD) != 0) device->hmd = static_cast<xrt_hmd_parts*>(std::calloc(1, sizeof(xrt_hmd_parts)));
  return device;
}
void u_device_free(xrt_device* device) {
  std::free(device->inputs);
  std::free(device->outputs);
  std::free(device->tracking_origin);
  std::free(device->hmd);
  std::free(device);
}
void u_device_populate_function_pointers(xrt_device* device,
                                        u_device_get_tracked_pose_function_t pose,
                                        u_device_destroy_function_t destroy) {
  device->get_tracked_pose = pose;
  device->destroy = destroy;
  device->update_inputs = u_device_noop_update_inputs;
}
void u_var_add_root(void*, const char*, bool) {}
void u_var_remove_root(void*) {}
bool u_device_setup_split_side_by_side(xrt_device*, const u_device_simple_info*) { return true; }
void u_distortion_mesh_set_none(xrt_device*) {}
xrt_result_t u_device_noop_update_inputs(xrt_device*) { return XRT_SUCCESS; }
xrt_result_t u_device_get_view_poses(xrt_device*, const xrt_vec3*, int64_t, xrt_view_type,
                                    uint32_t, xrt_space_relation*, xrt_fov*, xrt_pose*) {
  return XRT_SUCCESS;
}
}

using namespace xr::monado_driver;

template<class Base, class Sample>
struct SampleReader final : Base {
  Sample sample{};
  bool available = true;
  uint32_t reads = 0;
  bool read_latest(Sample& out, std::string*) override {
    ++reads;
    out = sample;
    out.read_time_ns = monotonic_now_ns();
    return available;
  }
  const char* transport_name() const override { return "test_samples"; }
};

struct Fixture {
  XrRuntimeIo io;
  XrTrackingDevice head{};
  SampleReader<IRuntimePoseReader, RuntimePoseSample>* hmd;
  SampleReader<IRuntimeControllerStateReader, RuntimeControllerStateSample>* controller;
  SampleReader<IRuntimeHandReader, RuntimeHandSample>* hand;
  xrt_device* left;
  xrt_device* right;

  explicit Fixture(bool hand_tracking = false) {
    if (hand_tracking) setenv("XR_MONADO_HAND_TRACKING", "1", 1);
    else unsetenv("XR_MONADO_HAND_TRACKING");
    hmd = new SampleReader<IRuntimePoseReader, RuntimePoseSample>();
    controller = new SampleReader<IRuntimeControllerStateReader, RuntimeControllerStateSample>();
    hand = new SampleReader<IRuntimeHandReader, RuntimeHandSample>();
    io.hmd_reader.reset(hmd);
    io.controller_reader.reset(controller);
    io.hand_reader.reset(hand);
    head.io = &io;
    left = xr_tracking_runtime_controller_create(&head.base, true);
    right = xr_tracking_runtime_controller_create(&head.base, false);
    assert(left && right);

    const auto now = static_cast<uint64_t>(monotonic_now_ns());
    auto& frame = controller->sample.frame;
    frame.timestamp_ns = now;
    frame.left.role = xr_runtime::CONTROLLER_SIDE_LEFT;
    frame.right.role = xr_runtime::CONTROLLER_SIDE_RIGHT;
    for (auto* side : {&frame.left, &frame.right}) {
      side->flags = xr_runtime::RUNTIME_CONTROLLER_CONNECTED | xr_runtime::RUNTIME_CONTROLLER_POSE_VALID |
                    xr_runtime::RUNTIME_CONTROLLER_INPUT_VALID | xr_runtime::RUNTIME_CONTROLLER_TRACKED;
      side->position[0] = 0.4f;
      side->position[1] = 1.3f;
      side->position[2] = -0.5f;
      side->linear_velocity[0] = 1.2f;
      side->angular_velocity[1] = 0.7f;
    }
    auto& hands = hand->sample.frame;
    hands.timestamp_ns = hands.source_timestamp_ns = now;
    hands.flags = xr_tracking::HAND_FLAG_LEFT_VALID | xr_tracking::HAND_FLAG_RIGHT_VALID |
                  xr_tracking::HAND_FLAG_JOINTS_VALID;
    hands.hand_count = 2;
    fill_hand(hands.left, 1);
    fill_hand(hands.right, 2);
  }
  ~Fixture() {
    left->destroy(left);
    right->destroy(right);
    unsetenv("XR_MONADO_HAND_TRACKING");
  }
  static void fill_hand(xr_tracking::HandSideF32V2& side, uint32_t handedness) {
    side.handedness = handedness;
    side.status = static_cast<uint32_t>(xr_tracking::HandTrackingStatus::Tracking);
    side.flags = xr_tracking::HAND_POSE_VALID | xr_tracking::HAND_JOINTS_VALID |
                 xr_tracking::HAND_LINEAR_VELOCITY_VALID | xr_tracking::HAND_ANGULAR_VELOCITY_VALID;
    side.confidence = 1.0f;
    side.controller_px = 0.3f;
    side.controller_py = 1.4f;
    side.controller_pz = -0.6f;
    side.vx = 0.9f;
    side.wy = 0.2f;
    side.palm_px = 0.2f;
    side.palm_py = 1.5f;
    side.palm_pz = -0.5f;
    // Palm orientation differs from wrist joints and must be preserved.
    side.palm_qw = 0.70710678f;
    side.palm_qz = 0.70710678f;
    for (uint32_t i = 0; i < xr_tracking::HAND_JOINT_COUNT_V2; ++i) {
      auto& joint = side.joints[i];
      joint.joint_id = i;
      joint.flags = kJointPoseValid | kJointPoseTracked;
      joint.confidence = 1.0f;
      joint.px = static_cast<float>(handedness) + 0.01f * static_cast<float>(i);
      joint.py = 1.5f;
      joint.pz = -0.5f - 0.01f * static_cast<float>(i);
    }
  }
};

xrt_input& input(xrt_device* device, xrt_input_name name) {
  for (uint32_t i = 0; i < device->input_count; ++i) {
    if (device->inputs[i].name == name) return device->inputs[i];
  }
  std::abort();
}

void test_default_and_opt_in() {
  {
    Fixture f;
    assert(f.left->name == XRT_DEVICE_TOUCH_CONTROLLER);
    assert(f.left->update_inputs == xrtd_update_inputs);
    assert(!f.left->supported.hand_tracking && !f.right->supported.hand_tracking);
    assert(f.left->input_count == ARRAY_SIZE(kControllerInputNames));
    xrt_hand_joint_set joints{};
    int64_t timestamp = 123;
    assert(xrtd_get_hand_tracking(f.left, XRT_INPUT_HT_UNOBSTRUCTED_LEFT, 0, &joints, &timestamp) == XRT_ERROR_INPUT_UNSUPPORTED);
    assert(!joints.is_active && timestamp == 0);
    for (uint32_t i = 0; i < f.left->binding_profile_count; ++i) {
      for (uint32_t j = 0; j < f.left->binding_profiles[i].input_count; ++j) {
        input(f.left, f.left->binding_profiles[i].inputs[j].device);
      }
    }
  }
  Fixture f(true);
  assert(f.left->supported.hand_tracking && f.right->supported.hand_tracking);
  assert(f.left->get_hand_tracking == xrtd_get_hand_tracking);
  assert(f.left->begin_feature(f.left, XRT_DEVICE_FEATURE_HAND_TRACKING_LEFT) == XRT_SUCCESS);
  assert(f.left->end_feature(f.left, XRT_DEVICE_FEATURE_HAND_TRACKING_LEFT) == XRT_SUCCESS);
  assert(f.right->begin_feature(f.right, XRT_DEVICE_FEATURE_HAND_TRACKING_RIGHT) == XRT_SUCCESS);
  assert(f.left->begin_feature(f.left, XRT_DEVICE_FEATURE_HAND_TRACKING_RIGHT) == XRT_ERROR_FEATURE_NOT_SUPPORTED);
  input(f.left, XRT_INPUT_HT_UNOBSTRUCTED_LEFT);
  input(f.right, XRT_INPUT_HT_UNOBSTRUCTED_RIGHT);
}

void test_buttons_analog_and_touchpad() {
  Fixture f;
  auto& side = f.controller->sample.frame.left;
  side.trigger = 0.63f;
  side.grip = 0.72f;
  side.buttons = xr_runtime::CONTROLLER_BUTTON_TRIGGER | xr_runtime::CONTROLLER_BUTTON_GRIP |
                 xr_runtime::CONTROLLER_BUTTON_X | xr_runtime::CONTROLLER_BUTTON_Y |
                 xr_runtime::CONTROLLER_BUTTON_A | xr_runtime::CONTROLLER_BUTTON_B;
  side.thumbstick_x = 0.25f;
  side.touches = xr_runtime::CONTROLLER_BUTTON_THUMBSTICK;
  assert(f.left->update_inputs(f.left) == XRT_SUCCESS);
  assert(input(f.left, XRT_INPUT_TOUCH_TRIGGER_VALUE).value.vec1.x == 0.63f);
  assert(input(f.left, XRT_INPUT_TOUCH_SQUEEZE_VALUE).value.vec1.x == 0.72f);
  assert(input(f.left, XRT_INPUT_WMR_SQUEEZE_CLICK).value.boolean);
  for (auto name : {XRT_INPUT_TOUCH_X_CLICK, XRT_INPUT_TOUCH_Y_CLICK, XRT_INPUT_TOUCH_A_CLICK, XRT_INPUT_TOUCH_B_CLICK}) {
    assert(input(f.left, name).value.boolean);
  }
  assert(input(f.left, XRT_INPUT_WMR_TRACKPAD_TOUCH).value.boolean);
  assert(input(f.left, XRT_INPUT_WMR_TRACKPAD_CLICK).value.boolean);
  assert(!input(f.left, XRT_INPUT_TOUCH_THUMBSTICK_CLICK).value.boolean);
  assert(input(f.left, XRT_INPUT_TOUCH_TRIGGER_VALUE).timestamp == static_cast<int64_t>(f.controller->sample.frame.timestamp_ns));
  side.trigger = side.grip = side.thumbstick_x = 0.0f;
  side.touches = 0;
  side.buttons = xr_runtime::CONTROLLER_BUTTON_DPAD_DOWN | xr_runtime::CONTROLLER_BUTTON_SYSTEM;
  f.left->update_inputs(f.left);
  assert(input(f.left, XRT_INPUT_TOUCH_THUMBSTICK).value.vec2.y == -1.0f);
  assert(input(f.left, XRT_INPUT_WMR_TRACKPAD_CLICK).value.boolean);
  assert(input(f.left, XRT_INPUT_TOUCH_SYSTEM_CLICK).value.boolean);
  assert(!input(f.left, XRT_INPUT_TOUCH_MENU_CLICK).value.boolean);
  side.buttons = xr_runtime::CONTROLLER_BUTTON_TRIGGER;
  f.left->update_inputs(f.left);
  assert(input(f.left, XRT_INPUT_TOUCH_TRIGGER_VALUE).value.vec1.x == 1.0f);
  assert(!input(f.left, XRT_INPUT_WMR_TRACKPAD_TOUCH).value.boolean);
  side.buttons = 0;
  side.flags &= ~xr_runtime::RUNTIME_CONTROLLER_INPUT_VALID;
  f.left->update_inputs(f.left);
  assert(input(f.left, XRT_INPUT_TOUCH_GRIP_POSE).active);
  assert(!input(f.left, XRT_INPUT_TOUCH_TRIGGER_VALUE).active);
}

void test_absolute_pose_and_authoritative_invalid() {
  Fixture f;
  xrt_space_relation relation{};
  f.left->get_tracked_pose(f.left, XRT_INPUT_TOUCH_GRIP_POSE, 0, &relation);
  assert(relation.pose.position.x == 0.4f);
  assert(relation.linear_velocity.x == 1.2f && relation.angular_velocity.y == 0.7f);
  f.controller->available = false;
  f.left->get_tracked_pose(f.left, XRT_INPUT_WMR_GRIP_POSE, 0, &relation);
  assert(relation.pose.position.x == 0.3f && relation.pose.position.y == 1.4f);
  assert(relation.linear_velocity.x == 0.9f && relation.angular_velocity.y == 0.2f);
  assert(f.hmd->reads == 0);  // World-space fallback needs no HMD sample.
  f.controller->available = true;
  f.controller->sample.frame.left.flags &= ~xr_runtime::RUNTIME_CONTROLLER_POSE_VALID;
  f.left->get_tracked_pose(f.left, XRT_INPUT_TOUCH_GRIP_POSE, 0, &relation);
  assert(relation.relation_flags == 0);
  f.controller->sample.frame.left.flags = 0;
  f.controller->sample.frame.left.buttons = xr_runtime::CONTROLLER_BUTTON_TRIGGER;
  f.left->update_inputs(f.left);
  assert(!input(f.left, XRT_INPUT_TOUCH_TRIGGER_VALUE).active);
  assert(!input(f.left, XRT_INPUT_TOUCH_GRIP_POSE).active);
}

void test_skeleton_mapping_and_loss() {
  Fixture f(true);
  xrt_hand_joint_set joints{};
  int64_t timestamp = 0;
  assert(f.left->get_hand_tracking(f.left, XRT_INPUT_HT_UNOBSTRUCTED_LEFT, monotonic_now_ns(), &joints, &timestamp) == XRT_SUCCESS);
  assert(joints.is_active && timestamp == static_cast<int64_t>(f.hand->sample.frame.source_timestamp_ns));
  auto& values = joints.values.hand_joint_set_default;
  assert(values[XRT_HAND_JOINT_WRIST].relation.pose.position.x == f.hand->sample.frame.left.joints[0].px);
  assert(values[XRT_HAND_JOINT_INDEX_TIP].relation.pose.position.x == f.hand->sample.frame.left.joints[8].px);
  assert(values[XRT_HAND_JOINT_LITTLE_TIP].relation.pose.position.x == f.hand->sample.frame.left.joints[20].px);
  assert(std::abs(values[XRT_HAND_JOINT_PALM].relation.pose.orientation.w - 0.70710678f) < 1e-6f);
  assert(std::abs(values[XRT_HAND_JOINT_PALM].relation.pose.orientation.z - 0.70710678f) < 1e-6f);
  assert(joints.hand_pose.pose.position.x == 0.0f && joints.hand_pose.pose.orientation.w == 1.0f);
  assert((values[XRT_HAND_JOINT_INDEX_METACARPAL].relation.relation_flags & kJointPoseTracked) == 0);
  assert(values[XRT_HAND_JOINT_INDEX_METACARPAL].radius > 0.0f);
  assert((values[XRT_HAND_JOINT_INDEX_TIP].relation.relation_flags & XRT_SPACE_RELATION_LINEAR_VELOCITY_VALID_BIT) == 0);
  f.right->get_hand_tracking(f.right, XRT_INPUT_HT_UNOBSTRUCTED_RIGHT, 0, &joints, &timestamp);
  assert(joints.is_active && joints.values.hand_joint_set_default[XRT_HAND_JOINT_WRIST].relation.pose.position.x == 2.0f);
  assert(f.left->get_hand_tracking(f.left, XRT_INPUT_HT_UNOBSTRUCTED_RIGHT, 0, &joints, &timestamp) == XRT_ERROR_INPUT_UNSUPPORTED);
  assert(!joints.is_active);

  auto inactive = [&] {
    f.left->get_hand_tracking(f.left, XRT_INPUT_HT_UNOBSTRUCTED_LEFT, 0, &joints, &timestamp);
    assert(!joints.is_active && timestamp == 0);
    f.left->update_inputs(f.left);
    assert(!input(f.left, XRT_INPUT_HT_UNOBSTRUCTED_LEFT).active);
  };
  f.hand->sample.frame.left.status = static_cast<uint32_t>(xr_tracking::HandTrackingStatus::Degraded);
  inactive();  // A held/predicted controller is not an observed finger skeleton.
  f.hand->sample.frame.left.status = static_cast<uint32_t>(xr_tracking::HandTrackingStatus::Tracking);
  f.hand->sample.frame.source_timestamp_ns -= 500000000ull;
  inactive();  // Freshly republished frame with an expired optical source.
  f.hand->sample.frame.source_timestamp_ns = static_cast<uint64_t>(monotonic_now_ns());
  f.hand->sample.frame.left.joint_count = 0;
  inactive();
  f.hand->sample.frame.left.joint_count = xr_tracking::HAND_JOINT_COUNT_V2;
  f.hand->available = false;
  inactive();
}

void test_palm_runtime_orientation() {
  Fixture f(true);
  const xrt_quat samples[] = {
      {0.0f, 0.0f, 0.0f, 1.0f},
      {0.70710678f, 0.0f, 0.0f, 0.70710678f},
      {0.0f, 0.70710678f, 0.0f, 0.70710678f},
      {0.0f, 0.0f, 0.70710678f, 0.70710678f},
      {0.0f, 1.0f, 0.0f, 0.0f},
      {0.4f, -0.2f, 0.6f, 1.2f},  // Non-unit input: preserve rotation while normalizing.
  };
  for (auto* device : {f.left, f.right}) {
    const bool left = device == f.left;
    const auto name = left ? XRT_INPUT_HT_UNOBSTRUCTED_LEFT : XRT_INPUT_HT_UNOBSTRUCTED_RIGHT;
    auto& side = left ? f.hand->sample.frame.left : f.hand->sample.frame.right;
    // Distinct wrist-joint, side-level wrist and controller orientations make
    // substitution of any of them observable independently of palm_q.
    side.joints[0].qw = side.joints[0].qx = 0.70710678f;
    side.wrist_qw = 0.0f;
    side.wrist_qz = 1.0f;
    side.controller_qw = 0.0f;
    side.controller_qy = 1.0f;
    xrt_hand_joint_set joints{};
    int64_t timestamp = 0;
    for (const auto& sample : samples) {
      side.palm_qw = sample.w;
      side.palm_qx = sample.x;
      side.palm_qy = sample.y;
      side.palm_qz = sample.z;
      assert(device->get_hand_tracking(device, name, 0, &joints, &timestamp) == XRT_SUCCESS);
      assert(joints.is_active);
      const auto& palm = joints.values.hand_joint_set_default[XRT_HAND_JOINT_PALM].relation;
      const auto expected = quat_normalize(sample);
      assert(std::abs(palm.pose.orientation.w - expected.w) < 1e-6f);
      assert(std::abs(palm.pose.orientation.x - expected.x) < 1e-6f);
      assert(std::abs(palm.pose.orientation.y - expected.y) < 1e-6f);
      assert(std::abs(palm.pose.orientation.z - expected.z) < 1e-6f);
      assert((palm.relation_flags & kJointPoseValid) == kJointPoseValid);
      assert(palm.pose.position.x == side.palm_px && palm.pose.position.y == side.palm_py &&
             palm.pose.position.z == side.palm_pz);
      const auto& wrist = joints.values.hand_joint_set_default[XRT_HAND_JOINT_WRIST].relation;
      assert(std::abs(wrist.pose.orientation.x - 0.70710678f) < 1e-6f);
      assert(joints.values.hand_joint_set_default[XRT_HAND_JOINT_INDEX_TIP].relation.pose.orientation.w == 1.0f);
    }
    for (float invalid : {0.0f, std::numeric_limits<float>::quiet_NaN(),
                           std::numeric_limits<float>::infinity()}) {
      side.palm_qw = invalid;
      side.palm_qx = side.palm_qy = side.palm_qz = 0.0f;
      assert(device->get_hand_tracking(device, name, 0, &joints, &timestamp) == XRT_SUCCESS);
      assert(joints.is_active);
      const auto& palm = joints.values.hand_joint_set_default[XRT_HAND_JOINT_PALM].relation;
      assert((palm.relation_flags & XRT_SPACE_RELATION_ORIENTATION_VALID_BIT) == 0);
      assert((palm.relation_flags & XRT_SPACE_RELATION_POSITION_VALID_BIT) != 0);
      assert(palm.pose.orientation.w == 1.0f && palm.pose.orientation.x == 0.0f);
      assert((joints.values.hand_joint_set_default[XRT_HAND_JOINT_INDEX_TIP].relation.relation_flags &
              kJointPoseValid) == kJointPoseValid);
    }
  }
}

void test_invalid_joints() {
  Fixture f(true);
  auto& side = f.hand->sample.frame.left;
  side.joints[8].px = std::numeric_limits<float>::quiet_NaN();
  side.joints[12].qw = 0.0f;
  side.joints[16].flags = 0;
  side.joints[20].confidence = 0.0f;
  xrt_hand_joint_set joints{};
  assert(runtime_hand_to_joint_set(side, joints));
  for (auto name : {XRT_HAND_JOINT_INDEX_TIP, XRT_HAND_JOINT_MIDDLE_TIP, XRT_HAND_JOINT_RING_TIP, XRT_HAND_JOINT_LITTLE_TIP}) {
    const auto& joint = joints.values.hand_joint_set_default[name];
    assert(joint.relation.relation_flags == 0 && std::isfinite(joint.relation.pose.position.x));
  }
  side.joints[0].qw = 0.0f;
  assert(!runtime_hand_to_joint_set(side, joints) && !joints.is_active);
}

void test_stream_loss_and_resume() {
  Fixture f(true);
  f.head.is_hmd = true;
  f.hmd->sample.pose.flags = xr_runtime::RUNTIME_HMD_FLAG_POSE_VALID;
  f.hmd->sample.pose.tracking_status = 2;
  f.hmd->sample.pose.confidence = 1.0f;
  f.hmd->sample.pose.px = 0.25;
  f.controller->sample.frame.left.buttons = xr_runtime::CONTROLLER_BUTTON_TRIGGER;
  f.left->update_inputs(f.left);
  assert(input(f.left, XRT_INPUT_TOUCH_TRIGGER_VALUE).value.vec1.x == 1.0f);
  xrt_space_relation relation{};
  xrtd_get_tracked_pose(&f.head.base, XRT_INPUT_GENERIC_HEAD_POSE, 0, &relation);
  assert(relation.pose.position.x == 0.25f);

  f.hmd->available = f.controller->available = f.hand->available = false;
  f.left->update_inputs(f.left);
  assert(!input(f.left, XRT_INPUT_TOUCH_TRIGGER_VALUE).active);
  assert(input(f.left, XRT_INPUT_TOUCH_TRIGGER_VALUE).value.vec1.x == 0.0f);
  f.left->get_tracked_pose(f.left, XRT_INPUT_TOUCH_GRIP_POSE, 0, &relation);
  assert(relation.relation_flags == 0);
  xrt_hand_joint_set joints{};
  int64_t timestamp = 0;
  f.left->get_hand_tracking(f.left, XRT_INPUT_HT_UNOBSTRUCTED_LEFT, 0, &joints, &timestamp);
  assert(!joints.is_active && timestamp == 0);
  xrtd_get_tracked_pose(&f.head.base, XRT_INPUT_GENERIC_HEAD_POSE, 0, &relation);
  assert(relation.pose.position.x == 0.0f && relation.pose.orientation.w == 1.0f);

  f.hmd->available = f.controller->available = f.hand->available = true;
  f.hmd->sample.pose.px = -0.5;
  f.controller->sample.frame.sequence = f.hand->sample.frame.sequence = 1;
  f.controller->sample.frame.left.buttons = 0;
  f.controller->sample.frame.left.thumbstick_x = -0.75f;
  f.hand->sample.frame.source_timestamp_ns = static_cast<uint64_t>(monotonic_now_ns());
  f.left->update_inputs(f.left);
  assert(input(f.left, XRT_INPUT_TOUCH_TRIGGER_VALUE).active);
  assert(input(f.left, XRT_INPUT_TOUCH_TRIGGER_VALUE).value.vec1.x == 0.0f);
  assert(input(f.left, XRT_INPUT_TOUCH_THUMBSTICK).value.vec2.x == -0.75f);
  f.left->get_tracked_pose(f.left, XRT_INPUT_TOUCH_GRIP_POSE, 0, &relation);
  assert(relation.pose.position.x == 0.4f);
  f.left->get_hand_tracking(f.left, XRT_INPUT_HT_UNOBSTRUCTED_LEFT, 0, &joints, &timestamp);
  assert(joints.is_active && timestamp != 0);
  xrtd_get_tracked_pose(&f.head.base, XRT_INPUT_GENERIC_HEAD_POSE, 0, &relation);
  assert(relation.pose.position.x == -0.5f);
}

void test_parallel_callbacks() {
  Fixture f(true);
  auto run = [&](xrt_device* device, xrt_input_name name) {
    for (int i = 0; i < 50; ++i) {
      xrt_hand_joint_set joints{};
      xrt_space_relation relation{};
      int64_t timestamp = 0;
      assert(device->update_inputs(device) == XRT_SUCCESS);
      assert(device->get_hand_tracking(device, name, 0, &joints, &timestamp) == XRT_SUCCESS);
      assert(joints.is_active);
      assert(device->get_tracked_pose(device, XRT_INPUT_TOUCH_GRIP_POSE, 0, &relation) == XRT_SUCCESS);
    }
  };
  std::thread left(run, f.left, XRT_INPUT_HT_UNOBSTRUCTED_LEFT);
  std::thread right(run, f.right, XRT_INPUT_HT_UNOBSTRUCTED_RIGHT);
  left.join();
  right.join();
}

int main() {
  test_default_and_opt_in();
  test_buttons_analog_and_touchpad();
  test_absolute_pose_and_authoritative_invalid();
  test_skeleton_mapping_and_loss();
  test_palm_runtime_orientation();
  test_invalid_joints();
  test_stream_loss_and_resume();
  test_parallel_callbacks();
  std::cout << "Monado driver: 8 test groups passed\n";
}
