#include <openvr_driver.h>
#include <nlohmann/json.hpp>

#include <cstring>
#include <cmath>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>

#ifndef _WIN32
#include <algorithm>
#include <array>
#include <filesystem>
#include <vector>
#include <xr_runtime/publishers/runtime_controller_state_shm_publisher.hpp>
#include <xr_tracking/contracts/body_tracker_set_contract.hpp>
#include <xr_tracking/publishers/hand_tracking_shm_publisher.hpp>
#endif

extern "C" void* HmdDriverFactory(const char*, int*);

namespace {
void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

// Exercise the actual driver factory and device lifecycle with a SteamVR host
// double. No runtime process, USB device or graphics adapter is required.
class Host : public vr::IVRDriverContext, public vr::IVRSettings,
                   public vr::IVRProperties, public vr::IVRDriverInput,
                   public vr::IVRServerDriverHost, public vr::IVRDriverLog,
                   public vr::IVRDriverManager, public vr::IVRResources {
 public:
  static constexpr vr::PropertyContainerHandle_t container = 100;
  vr::ITrackedDeviceServerDriver* hmd = nullptr;
  std::map<vr::ETrackedDeviceProperty, nlohmann::json> properties;
  vr::VRInputComponentHandle_t next_handle = 1, proximity_handle = 0;
  unsigned updates = 0, poses = 0;
  bool fail_proximity = false;

  void* GetGenericInterface(const char* name, vr::EVRInitError* error) override {
    if (error) *error = vr::VRInitError_None;
#define INTERFACE(type) if (std::strcmp(name, vr::type##_Version) == 0) return static_cast<vr::type*>(this)
    INTERFACE(IVRSettings);
    INTERFACE(IVRProperties);
    INTERFACE(IVRDriverInput);
    INTERFACE(IVRServerDriverHost);
    INTERFACE(IVRDriverLog);
    INTERFACE(IVRDriverManager);
    INTERFACE(IVRResources);
#undef INTERFACE
    if (error) *error = vr::VRInitError_Init_InterfaceNotFound;
    return nullptr;
  }
  vr::DriverHandle_t GetDriverHandle() override { return 1; }

  const char* GetSettingsErrorNameFromEnum(vr::EVRSettingsError) override { return "unset"; }
  void SetBool(const char*, const char*, bool, vr::EVRSettingsError*) override {}
  void SetInt32(const char*, const char*, int32_t, vr::EVRSettingsError*) override {}
  void SetFloat(const char*, const char*, float, vr::EVRSettingsError*) override {}
  void SetString(const char*, const char*, const char*, vr::EVRSettingsError*) override {}
  bool GetBool(const char*, const char*, vr::EVRSettingsError* error) override { missing(error); return false; }
  int32_t GetInt32(const char*, const char*, vr::EVRSettingsError* error) override { missing(error); return 0; }
  float GetFloat(const char*, const char*, vr::EVRSettingsError* error) override { missing(error); return 0; }
  void GetString(const char*, const char* key, char* value, uint32_t size, vr::EVRSettingsError* error) override {
    missing(error);
    // Keep runtime data unavailable to check presence independently of pose validity.
    const char* text = std::strcmp(key, "runtimePoseTransport") == 0 ? "none" : "";
    if (size) { std::strncpy(value, text, size - 1); value[size - 1] = '\0'; }
  }
  void RemoveSection(const char*, vr::EVRSettingsError*) override {}
  void RemoveKeyInSection(const char*, const char*, vr::EVRSettingsError*) override {}

  vr::PropertyContainerHandle_t TrackedDeviceToPropertyContainer(vr::TrackedDeviceIndex_t device) override {
    require(device == 0, "unexpected device property container");
    return container;
  }
  const char* GetPropErrorNameFromEnum(vr::ETrackedPropertyError) override { return "property error"; }
  vr::ETrackedPropertyError ReadPropertyBatch(vr::PropertyContainerHandle_t, vr::PropertyRead_t*, uint32_t) override {
    return vr::TrackedProp_UnknownProperty;
  }
  vr::ETrackedPropertyError WritePropertyBatch(vr::PropertyContainerHandle_t target,
                                              vr::PropertyWrite_t* batch, uint32_t count) override {
    require(target == container, "properties written to wrong device");
    for (uint32_t i = 0; i < count; ++i) {
      auto& property = batch[i];
      require(property.writeType == vr::PropertyWrite_Set, "unexpected property operation");
      auto& value = properties[property.prop];
      switch (property.unTag) {
        case vr::k_unStringPropertyTag: value = static_cast<const char*>(property.pvBuffer); break;
        case vr::k_unBoolPropertyTag: value = *static_cast<bool*>(property.pvBuffer); break;
        case vr::k_unFloatPropertyTag: value = *static_cast<float*>(property.pvBuffer); break;
        case vr::k_unInt32PropertyTag: value = *static_cast<int32_t*>(property.pvBuffer); break;
        case vr::k_unUint64PropertyTag: value = *static_cast<uint64_t*>(property.pvBuffer); break;
        default: throw std::runtime_error("unexpected property type");
      }
      property.eError = vr::TrackedProp_Success;
    }
    return vr::TrackedProp_Success;
  }

  vr::EVRInputError CreateBooleanComponent(vr::PropertyContainerHandle_t target, const char* path,
                                          vr::VRInputComponentHandle_t* handle) override {
    require(target == container, "presence component belongs to wrong device");
    require(std::strcmp(path, "/proximity") == 0, "HMD presence must use the reserved /proximity input");
    if (fail_proximity) return vr::VRInputError_InvalidParam;
    *handle = proximity_handle = next_handle++;
    return vr::VRInputError_None;
  }
  vr::EVRInputError UpdateBooleanComponent(vr::VRInputComponentHandle_t handle, bool value, double offset) override {
    require(handle != 0 && handle == proximity_handle, "stale or invalid presence component");
    require(value && offset == 0.0, "active HMD must report present even without runtime pose data");
    ++updates;
    return vr::VRInputError_None;
  }
  vr::EVRInputError CreateScalarComponent(vr::PropertyContainerHandle_t, const char*, vr::VRInputComponentHandle_t*, vr::EVRScalarType, vr::EVRScalarUnits) override { return vr::VRInputError_InvalidParam; }
  vr::EVRInputError UpdateScalarComponent(vr::VRInputComponentHandle_t, float, double) override { return vr::VRInputError_InvalidParam; }
  vr::EVRInputError CreateHapticComponent(vr::PropertyContainerHandle_t, const char*, vr::VRInputComponentHandle_t*) override { return vr::VRInputError_InvalidParam; }
  vr::EVRInputError CreateSkeletonComponent(vr::PropertyContainerHandle_t, const char*, const char*, const char*, vr::EVRSkeletalTrackingLevel, const vr::VRBoneTransform_t*, uint32_t, vr::VRInputComponentHandle_t*) override { return vr::VRInputError_InvalidParam; }
  vr::EVRInputError UpdateSkeletonComponent(vr::VRInputComponentHandle_t, vr::EVRSkeletalMotionRange, const vr::VRBoneTransform_t*, uint32_t) override { return vr::VRInputError_InvalidParam; }
  vr::EVRInputError CreatePoseComponent(vr::PropertyContainerHandle_t, const char*, vr::VRInputComponentHandle_t*) override { return vr::VRInputError_InvalidParam; }
  vr::EVRInputError UpdatePoseComponent(vr::VRInputComponentHandle_t, const vr::HmdMatrix34_t*, double) override { return vr::VRInputError_InvalidParam; }
  vr::EVRInputError CreateEyeTrackingComponent(vr::PropertyContainerHandle_t, const char*, vr::VRInputComponentHandle_t*) override { return vr::VRInputError_InvalidParam; }
  vr::EVRInputError UpdateEyeTrackingComponent(vr::VRInputComponentHandle_t, const vr::VREyeTrackingData_t*, double) override { return vr::VRInputError_InvalidParam; }

  bool TrackedDeviceAdded(const char*, vr::ETrackedDeviceClass type, vr::ITrackedDeviceServerDriver* device) override {
    require(type == vr::TrackedDeviceClass_HMD && hmd == nullptr, "expected one HMD");
    hmd = device;
    return true;
  }
  void TrackedDevicePoseUpdated(uint32_t device, const vr::DriverPose_t& pose, uint32_t size) override {
    require(device == 0 && size == sizeof(pose), "invalid pose submission");
    require(pose.deviceIsConnected && !pose.poseIsValid, "unavailable tracking must remain invalid");
    ++poses;
  }
  void VsyncEvent(double) override {}
  void VendorSpecificEvent(uint32_t, vr::EVREventType, const vr::VREvent_Data_t&, double) override {}
  bool IsExiting() override { return false; }
  bool PollNextEvent(vr::VREvent_t*, uint32_t) override { return false; }
  void GetRawTrackedDevicePoses(float, vr::TrackedDevicePose_t*, uint32_t) override {}
  void RequestRestart(const char*, const char*, const char*, const char*) override {}
  uint32_t GetFrameTimings(vr::Compositor_FrameTiming*, uint32_t) override { return 0; }
  void SetDisplayEyeToHead(uint32_t, const vr::HmdMatrix34_t&, const vr::HmdMatrix34_t&) override {}
  void SetDisplayProjectionRaw(uint32_t, const vr::HmdRect2_t&, const vr::HmdRect2_t&) override {}
  void SetRecommendedRenderTargetSize(uint32_t, uint32_t, uint32_t) override {}
  void Log(const char*) override {}
  uint32_t GetDriverCount() const override { return 1; }
  uint32_t GetDriverName(vr::DriverId_t, char*, uint32_t) override { return 0; }
  vr::DriverHandle_t GetDriverHandle(const char*) override { return 1; }
  bool IsEnabled(vr::DriverId_t) const override { return true; }
  uint32_t LoadSharedResource(const char*, char*, uint32_t) override { return 0; }
  uint32_t GetResourceFullPath(const char*, const char*, char*, uint32_t) override { return 0; }

 private:
  static void missing(vr::EVRSettingsError* error) { if (error) *error = vr::VRSettingsError_UnsetSettingHasNoDefault; }
};

void check_universe(const Host& host) {
  const auto document = nlohmann::json::parse(
      host.properties.at(vr::Prop_DriverProvidedChaperoneJson_String).get<std::string>());
  require(document.at("jsonid") == "chaperone_info", "chaperone format identifier rejected by SteamVR");
  require(document.at("version") == 5, "unexpected chaperone version");
  const auto universe_id = host.properties.at(vr::Prop_CurrentUniverseId_Uint64).get<uint64_t>();
  require(universe_id > 1, "HMD needs its own tracking universe");
  const auto& universes = document.at("universes");
  require(universes.size() == 1, "expected a single driver tracking universe");
  const auto& universe = universes.at(0);
  require(universe.at("universeID").get<uint64_t>() == universe_id, "chaperone and active HMD universe disagree");
  for (const auto* origin : {"seated", "standing"}) {
    require(universe.at(origin).at("translation").size() == 3, "tracking origin must have three coordinates");
    require(universe.at(origin).at("yaw").is_number(), "tracking origin needs yaw");
  }
  const auto& area = universe.at("play_area");
  require(area.size() == 2 && area[0].get<double>() > 0 && area[1].get<double>() > 0, "invalid play area");
}

void check_display(vr::ITrackedDeviceServerDriver& hmd) {
  auto* display = static_cast<vr::IVRDisplayComponent*>(hmd.GetComponent(vr::IVRDisplayComponent_Version));
  require(display != nullptr, "HMD did not expose its display component");
  uint32_t width = 0, height = 0;
  display->GetRecommendedRenderTargetSize(&width, &height);
  require(width > 0 && height > 0, "invalid render target dimensions");
  // Call through the SDK interface, including the aggregate return used by
  // SteamVR when it builds the distortion mesh during compositor startup.
  for (const auto eye : {vr::Eye_Left, vr::Eye_Right}) {
    const auto distortion = display->ComputeDistortion(eye, 0.25f, 0.75f);
    for (const auto* channel : {distortion.rfRed, distortion.rfGreen, distortion.rfBlue}) {
      require(std::abs(channel[0] - 0.25f) < 1e-6f && std::abs(channel[1] - 0.75f) < 1e-6f,
              "default display distortion changed texture coordinates");
    }
    vr::HmdVector2_t inverse{};
    require(display->ComputeInverseDistortion(&inverse, eye, 0, distortion.rfRed[0], distortion.rfRed[1]),
            "inverse display distortion failed");
    require(std::abs(inverse.v[0] - 0.25f) < 1e-6f && std::abs(inverse.v[1] - 0.75f) < 1e-6f,
            "display distortion did not round trip");
  }
}

#ifndef _WIN32
class TrackingHost final : public Host {
 public:
  struct Device {
    std::string serial;
    vr::ETrackedDeviceClass type;
    vr::DriverPose_t pose{};
  };

  bool opt_in = false;
  bool allow_predicted_skeleton = false;
  std::map<std::string, std::string> settings;
  std::vector<Device> devices;
  unsigned skeleton_count = 0;
  unsigned skeleton_updates = 0;

  bool GetBool(const char* section, const char* key, vr::EVRSettingsError* error) override {
    if ((opt_in && std::strcmp(key, "bodyTrackersEnabled") == 0) ||
        (allow_predicted_skeleton && std::strcmp(key, "handSkeletonAllowPredicted") == 0)) {
      if (error) *error = vr::VRSettingsError_None;
      return true;
    }
    return Host::GetBool(section, key, error);
  }

  void GetString(const char* section, const char* key, char* value, uint32_t size,
                 vr::EVRSettingsError* error) override {
    const auto it = settings.find(key);
    if (it == settings.end()) {
      Host::GetString(section, key, value, size, error);
      return;
    }
    if (error) *error = vr::VRSettingsError_None;
    if (size) {
      std::strncpy(value, it->second.c_str(), size - 1);
      value[size - 1] = '\0';
    }
  }

  vr::PropertyContainerHandle_t TrackedDeviceToPropertyContainer(vr::TrackedDeviceIndex_t index) override {
    return container + index;
  }

  vr::ETrackedPropertyError WritePropertyBatch(vr::PropertyContainerHandle_t target,
                                              vr::PropertyWrite_t* batch, uint32_t count) override {
    const auto& device = devices.at(target - container - 1);
    for (uint32_t i = 0; i < count; ++i) {
      auto& property = batch[i];
      if (device.type == vr::TrackedDeviceClass_Controller && property.prop == vr::Prop_InputProfilePath_String) {
        const std::string profile = static_cast<const char*>(property.pvBuffer);
        require((profile.find("_skeleton_profile.json") != std::string::npos) == opt_in,
                "skeleton profile must be selected only when enabled");
      }
      property.eError = vr::TrackedProp_Success;
    }
    return vr::TrackedProp_Success;
  }

  vr::EVRInputError CreateBooleanComponent(vr::PropertyContainerHandle_t, const char*,
                                          vr::VRInputComponentHandle_t*) override {
    return vr::VRInputError_InvalidParam;
  }

  vr::EVRInputError CreateSkeletonComponent(vr::PropertyContainerHandle_t target, const char* path,
      const char* skeleton, const char* base, vr::EVRSkeletalTrackingLevel,
      const vr::VRBoneTransform_t*, uint32_t, vr::VRInputComponentHandle_t* handle) override {
    const bool left = target == container + 1;
    require(opt_in && std::strcmp(base, "/pose/raw") == 0, "unexpected hand skeleton registration");
    require(std::strcmp(path, left ? "/input/skeleton/left" : "/input/skeleton/right") == 0 &&
            std::strcmp(skeleton, left ? "/skeleton/hand/left" : "/skeleton/hand/right") == 0,
            "incorrect hand skeleton paths");
    *handle = target;
    ++skeleton_count;
    return vr::VRInputError_None;
  }

  vr::EVRInputError UpdateSkeletonComponent(vr::VRInputComponentHandle_t handle, vr::EVRSkeletalMotionRange range,
      const vr::VRBoneTransform_t* bones, uint32_t count) override {
    require(count == 31, "SteamVR hand skeleton requires 31 bones");
    require(bones[0].position.v[0] == 0.0f && bones[0].orientation.w == 1.0f, "root is not at /pose/raw");
    const bool left = handle == container + 1;
    const float sign = left ? -1.0f : 1.0f;
    // The controller-state position differs from the hand stream's own pose.
    require(std::abs(bones[1].position.v[0] - sign * 0.125f) < 1e-5f, "wrist offset uses the wrong pose");
    require(std::abs(bones[26].position.v[2] + 0.0625f) < 1e-5f, "thumb auxiliary bone must use distal position");
    require(std::abs(bones[6].position.v[2] - 0.015625f) < 1e-5f, "index metacarpal is not parent-relative");
    require(range == vr::VRSkeletalMotionRange_WithController || range == vr::VRSkeletalMotionRange_WithoutController,
            "invalid skeleton motion range");
    for (uint32_t i = 0; i < count; ++i) {
      const auto& q = bones[i].orientation;
      require(bones[i].position.v[3] == 1.0f && std::abs(q.w*q.w + q.x*q.x + q.y*q.y + q.z*q.z - 1.0f) < 1e-5f,
              "non-normalized skeleton transform");
    }
    ++skeleton_updates;
    return vr::VRInputError_None;
  }

  bool TrackedDeviceAdded(const char* serial, vr::ETrackedDeviceClass type,
                          vr::ITrackedDeviceServerDriver* device) override {
    if (type == vr::TrackedDeviceClass_HMD) return Host::TrackedDeviceAdded(serial, type, device);
    devices.push_back({serial, type});
    require(device->Activate(uint32_t(devices.size())) == vr::VRInitError_None, "tracking device activation failed");
    return true;
  }

  void TrackedDevicePoseUpdated(uint32_t index, const vr::DriverPose_t& pose, uint32_t size) override {
    require(size == sizeof(pose), "wrong pose size");
    devices.at(index - 1).pose = pose;
  }
};

void check_tracking_streams(vr::IServerTrackedDeviceProvider& server) {
  using namespace xr_tracking;
  const std::string prefix = "/xr_openvr_streams_" + std::to_string(getpid());
  const auto registry = std::filesystem::temp_directory_path() / (prefix.substr(1) + ".json");
  struct Cleanup {
    std::string prefix;
    std::filesystem::path registry;
    ~Cleanup() {
      for (const char* suffix : {"_hands", "_controllers", "_body"}) shm_unlink((prefix + suffix).c_str());
      std::filesystem::remove(registry);
      std::filesystem::remove(registry.string() + ".lock");
    }
  } cleanup{prefix, registry};

  HandTrackingShmPublisherConfig hand_config;
  hand_config.shm_name = prefix + "_hands";
  hand_config.registry_path = registry.string();
  hand_config.slot_count = 2;
  HandTrackingShmPublisherV2 hand_publisher(hand_config);
  HandTrackingFrameF32V2 hands;
  hands.flags = HAND_FLAG_LEFT_VALID | HAND_FLAG_RIGHT_VALID | HAND_FLAG_JOINTS_VALID;
  hands.hand_count = 2;
  for (auto* hand : {&hands.left, &hands.right}) {
    const bool left = hand == &hands.left;
    hand->handedness = left ? 1 : 2;
    hand->status = uint32_t(HandTrackingStatus::Tracking);
    hand->flags = HAND_POSE_VALID | HAND_JOINTS_VALID;
    hand->confidence = 1.0f;
    for (uint32_t i = 0; i < HAND_JOINT_COUNT_V2; ++i) {
      auto& joint = hand->joints[i];
      joint.joint_id = i;
      joint.flags = 3;
      joint.px = left ? -0.375f : 0.375f;
      joint.py = 1.25f;
      joint.pz = i == 0 ? -0.5f : -0.5f - float((i - 1) % 4 + 1) * 0.03125f;
    }
    hand->joints[5].pz = -0.5625f;
  }

  xr_runtime::RuntimeControllerStateShmPublisherConfig controller_config;
  controller_config.shm_name = prefix + "_controllers";
  controller_config.registry_path = registry.string();
  controller_config.slot_count = 2;
  xr_runtime::RuntimeControllerStateShmPublisher controller_publisher(controller_config);
  xr_runtime::RuntimeControllerStateFrameV1 controllers;
  for (auto* side : {&controllers.left, &controllers.right}) {
    const bool left = side == &controllers.left;
    side->role = left ? xr_runtime::CONTROLLER_SIDE_LEFT : xr_runtime::CONTROLLER_SIDE_RIGHT;
    side->flags = xr_runtime::RUNTIME_CONTROLLER_CONNECTED | xr_runtime::RUNTIME_CONTROLLER_POSE_VALID;
    side->position[0] = left ? -0.25f : 0.25f;
    side->position[1] = 1.25f;
    side->position[2] = -0.53125f;
  }

  BodyTrackerSetShmPublisherConfig body_config;
  body_config.shm_name = prefix + "_body";
  body_config.registry_path = registry.string();
  body_config.slot_count = 2;
  BodyTrackerSetShmPublisher body_publisher(body_config);
  BodyTrackerSetFrameF32V1 body;
  body.source = BODY_TRACKER_SOURCE_FRAME;
  constexpr std::array ids{"frame_neck", "frame_left_shoulder", "frame_left_elbow", "frame_left_wrist",
      "frame_right_shoulder", "frame_right_elbow", "frame_right_wrist",
      "frame_left_hip", "frame_left_knee", "frame_left_foot", "frame_left_toe",
      "frame_right_hip", "frame_right_knee", "frame_right_foot", "frame_right_toe", "frame_waist"};
  body.tracker_count = ids.size();
  for (uint32_t i = 0; i < body.tracker_count; ++i) {
    auto& tracker = body.trackers[i];
    std::strcpy(tracker.tracker_id, ids[i]);
    tracker.role = BODY_TRACKER_ROLE_GENERIC;
    tracker.flags = BODY_TRACKER_FLAG_CONNECTED | BODY_TRACKER_FLAG_POSITION_VALID | BODY_TRACKER_FLAG_ROLE_VALID;
    tracker.status = BODY_TRACKER_STATUS_TRACKING;
    tracker.pose.py = float(i) * 0.125f;
  }
  body.trackers[0].status = BODY_TRACKER_STATUS_LOST;
  body.trackers[0].flags &= ~BODY_TRACKER_FLAG_POSITION_VALID;

  struct StreamTestMode {
    bool opt_in;
    bool allow_predicted;
    bool hand_only;
  };
  constexpr StreamTestMode modes[] = {
      {false, false, false}, {true, false, false}, {true, true, false}, {true, true, true}};
  for (const auto& [opt_in, allow_predicted, hand_only] : modes) {
    TrackingHost host;
    host.opt_in = opt_in;
    host.allow_predicted_skeleton = allow_predicted;
    if (hand_only) {
      for (auto* hand : {&hands.left, &hands.right}) {
        const auto& controller = hand == &hands.left ? controllers.left : controllers.right;
        hand->controller_px = controller.position[0];
        hand->controller_py = controller.position[1];
        hand->controller_pz = controller.position[2];
      }
    }
    host.settings = {{"handControllerMode", opt_in ? "skeleton" : "sticks"},
                     {"runtimeHandShmName", hand_config.shm_name},
                     {"runtimeControllerStateShmName", hand_only ? prefix + "_missing" : controller_config.shm_name},
                     {"runtimeBodyTrackersShmName", body_config.shm_name}};
    hand_publisher.publish(hands);
    controller_publisher.publish(controllers);
    body_publisher.publish(body);
    require(server.Init(&host) == vr::VRInitError_None, "stream test driver init failed");
    server.RunFrame();
    require(host.devices.size() == (opt_in ? 2 + ids.size() : 2), "wrong body tracker opt-in behavior");
    require(host.skeleton_count == (opt_in ? 2 : 0) &&
            host.skeleton_updates == (opt_in ? 8 : 0), "wrong skeletal opt-in behavior");
    if (opt_in) {
      for (size_t i = 0; i < ids.size(); ++i) {
        const auto& device = host.devices[2 + i];
        require(device.type == vr::TrackedDeviceClass_GenericTracker &&
                device.serial == std::string("xr-gate-body-id-") + ids[i], "FRAME identity or class was lost");
      }
      auto& neck = host.devices[2].pose;
      require(neck.deviceIsConnected && !neck.poseIsValid, "a lost neck must not have a usable pose");
      auto recovered_body = body;
      std::swap(recovered_body.trackers[0], recovered_body.trackers[1]);
      recovered_body.trackers[1].status = BODY_TRACKER_STATUS_TRACKING;
      recovered_body.trackers[1].flags |= BODY_TRACKER_FLAG_POSITION_VALID;
      recovered_body.trackers[1].pose.py = 1.5f;
      body_publisher.publish(recovered_body);
      const auto samples = host.skeleton_updates;
      hands.source_timestamp_ns = uint64_t(xr_tracking::now_ns()) - 500000000;
      hand_publisher.publish(hands);
      server.RunFrame();
      require(host.devices.size() == 2 + ids.size() && neck.poseIsValid && neck.vecPosition[1] == 1.5,
              "neck must recover on the same device after reordering");
      require(host.skeleton_updates == samples, "stale optical hands must not update skeletal input");

      hands.left.status = 2u;
      hand_publisher.publish(hands);
      server.RunFrame();
      const auto expected_updates = samples + (allow_predicted ? 2 : 0);
      require(host.skeleton_updates == expected_updates, "status 2 requires the predicted skeleton opt-in");

      hands.left.flags &= ~HAND_JOINTS_VALID;
      hand_publisher.publish(hands);
      server.RunFrame();
      require(host.skeleton_updates == expected_updates, "a predicted skeleton still needs valid joints");
      require(host.devices[0].pose.poseIsValid == !hand_only, "invalid joints cannot provide a predicted root pose");
      hands.left.flags |= HAND_JOINTS_VALID;
      hands.timestamp_ns = hands.source_timestamp_ns;
      hand_publisher.publish(hands);
      recovered_body.timestamp_ns = hands.timestamp_ns;
      body_publisher.publish(recovered_body);
      server.RunFrame();
      require(host.skeleton_updates == expected_updates, "stale predicted frames must not update skeletal input");
      require(!neck.deviceIsConnected && !neck.poseIsValid, "stale body stream must invalidate its devices");
      hands.timestamp_ns = hands.source_timestamp_ns = 0;
      hands.left.status = uint32_t(HandTrackingStatus::Tracking);
    }
    server.Cleanup();
  }
}
#endif
}  // namespace

int main() {
  try {
    Host host;
    int error = 0;
    auto* server = static_cast<vr::IServerTrackedDeviceProvider*>(
        HmdDriverFactory(vr::IServerTrackedDeviceProvider_Version, &error));
    require(server != nullptr, "driver factory did not expose the server");
    require(server->Init(&host) == vr::VRInitError_None && host.hmd, "HMD registration failed");
    require(host.hmd->Activate(0) == vr::VRInitError_None, "HMD activation failed");
    check_universe(host);
    check_display(*host.hmd);
    require(host.properties.at(vr::Prop_ContainsProximitySensor_Bool) == true, "HMD presence not advertised");
    require(host.updates == 1, "presence must be set during activation");
    for (unsigned i = 0; i < 120; ++i) server->RunFrame();
    require(host.updates == 121 && host.poses == 120, "presence must stay active while tracking is unavailable");

    const auto old_handle = host.proximity_handle;
    host.hmd->Deactivate();
    server->RunFrame();
    require(host.updates == 121 && host.poses == 120, "deactivated HMD must stop submitting");
    require(host.hmd->Activate(0) == vr::VRInitError_None, "HMD reactivation failed");
    require(host.proximity_handle != old_handle, "reactivation reused a stale input component");
    server->RunFrame();
    require(host.updates == 123 && host.poses == 121, "reactivated HMD did not resume presence");

    host.hmd->Deactivate();
    host.fail_proximity = true;
    const auto previous_updates = host.updates;
    require(host.hmd->Activate(0) == vr::VRInitError_None, "input failure should retain tracking fallback");
    require(host.properties.at(vr::Prop_ContainsProximitySensor_Bool) == false, "failed input must not advertise a presence sensor");
    server->RunFrame();
    require(host.updates == previous_updates, "failed presence input must not be updated");
    host.hmd->Deactivate();
    server->Cleanup();
    std::cout << "HMD presence and chaperone lifecycle passed\n";
#ifndef _WIN32
    check_tracking_streams(*server);
    std::cout << "OpenVR hand skeleton and FRAME opt-in lifecycle passed\n";
#endif
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
