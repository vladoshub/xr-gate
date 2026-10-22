#include <openvr_driver.h>
#include <nlohmann/json.hpp>

#include <cstring>
#include <cmath>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>

extern "C" void* HmdDriverFactory(const char*, int*);

namespace {
void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

// Exercise the actual driver factory and device lifecycle with a SteamVR host
// double. No runtime process, USB device or graphics adapter is required.
class Host final : public vr::IVRDriverContext, public vr::IVRSettings,
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
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
