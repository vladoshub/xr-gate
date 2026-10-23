// Headless OpenXR consumer. Uses CPU frames; a rendering application can upload
// the left/right bytes returned here to its own textures for video passthrough.
// Build: c++ -std=c++17 xr_gate_data_probe.cpp -I../include -lopenxr_loader -o
// xr_gate_data_probe
#include "spatial_compat.h"
#include "xr_gate_data.h"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>
static void check(XrResult r) {
  if (XR_FAILED(r))
    throw std::runtime_error("OpenXR error " + std::to_string(r));
}
template <class T> T get(XrInstance i, const char *n) {
  PFN_xrVoidFunction f;
  check(xrGetInstanceProcAddr(i, n, &f));
  return reinterpret_cast<T>(f);
}
int main() try {
  uint32_t n = 0;
  check(xrEnumerateInstanceExtensionProperties(nullptr, 0, &n, nullptr));
  std::vector<XrExtensionProperties> props(n, {XR_TYPE_EXTENSION_PROPERTIES});
  check(xrEnumerateInstanceExtensionProperties(nullptr, n, &n, props.data()));
  auto has = [&](const char *name) {
    return std::any_of(props.begin(), props.end(), [&](const auto &p) {
      return !std::strcmp(p.extensionName, name);
    });
  };
  if (!has(XR_MNDX_XR_GATE_DATA_EXTENSION_NAME))
    throw std::runtime_error("Set XR_MONADO_SPATIAL=1 and/or XR_MONADO_VIDEO=1 "
                             "in this application's environment");
  std::vector<const char *> ext{XR_MNDX_XR_GATE_DATA_EXTENSION_NAME,
                                XR_MND_HEADLESS_EXTENSION_NAME};
  const bool spatial = has(XR_EXT_SPATIAL_MESH_TRACKING_EXTENSION_NAME);
  if (spatial)
    ext.insert(ext.end(), {XR_EXT_FUTURE_EXTENSION_NAME,
                           XR_EXT_SPATIAL_ENTITY_EXTENSION_NAME,
                           XR_EXT_SPATIAL_MESH_TRACKING_EXTENSION_NAME});
  XrInstanceCreateInfo ci{XR_TYPE_INSTANCE_CREATE_INFO};
  ci.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;
  std::strcpy(ci.applicationInfo.applicationName, "XR Gate data probe");
  ci.enabledExtensionCount = uint32_t(ext.size());
  ci.enabledExtensionNames = ext.data();
  XrInstance instance;
  check(xrCreateInstance(&ci, &instance));
  XrSystemGetInfo system_info{XR_TYPE_SYSTEM_GET_INFO, nullptr,
                              XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY};
  XrSystemId system;
  check(xrGetSystem(instance, &system_info, &system));
  XrSessionCreateInfo si{XR_TYPE_SESSION_CREATE_INFO};
  si.systemId = system;
  XrSession session;
  check(xrCreateSession(instance, &si, &session));
  XrReferenceSpaceCreateInfo ri{XR_TYPE_REFERENCE_SPACE_CREATE_INFO,
                                nullptr,
                                XR_REFERENCE_SPACE_TYPE_LOCAL,
                                {{0, 0, 0, 1}, {0, 0, 0}}};
  XrSpace base;
  check(xrCreateReferenceSpace(session, &ri, &base));
  auto acquire =
      get<PFN_xrCreateDataSnapshotMNDX>(instance, "xrCreateDataSnapshotMNDX");
  auto release =
      get<PFN_xrDestroyDataSnapshotMNDX>(instance, "xrDestroyDataSnapshotMNDX");
  auto mesh = get<PFN_xrGetSpatialMeshMNDX>(instance, "xrGetSpatialMeshMNDX");
  auto video = get<PFN_xrGetStereoVideoMNDX>(instance, "xrGetStereoVideoMNDX");
  XrSpatialContextEXT context = XR_NULL_HANDLE;
  if (spatial) {
    const XrSpatialComponentTypeEXT components[] = {
        XR_SPATIAL_COMPONENT_TYPE_BOUNDED_3D_EXT,
        XR_SPATIAL_COMPONENT_TYPE_MESH_3D_EXT};
    XrSpatialCapabilityConfigurationMeshTrackingEXT cfg{
        XR_TYPE_SPATIAL_CAPABILITY_CONFIGURATION_MESH_TRACKING_EXT, nullptr,
        XR_SPATIAL_CAPABILITY_MESH_TRACKING_EXT, 2, components};
    const auto *config =
        reinterpret_cast<const XrSpatialCapabilityConfigurationBaseHeaderEXT *>(
            &cfg);
    XrSpatialContextCreateInfoEXT info{XR_TYPE_SPATIAL_CONTEXT_CREATE_INFO_EXT,
                                       nullptr, 1, &config};
    XrFutureEXT future;
    check(get<PFN_xrCreateSpatialContextAsyncEXT>(
        instance, "xrCreateSpatialContextAsyncEXT")(session, &info, &future));
    XrCreateSpatialContextCompletionEXT done{
        XR_TYPE_CREATE_SPATIAL_CONTEXT_COMPLETION_EXT};
    check(get<PFN_xrCreateSpatialContextCompleteEXT>(
        instance, "xrCreateSpatialContextCompleteEXT")(session, future, &done));
    check(done.futureResult);
    context = done.spatialContext;
  }
  bool running = false, exit = false;
  const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (!exit && std::chrono::steady_clock::now() < end) {
    XrEventDataBuffer ev{XR_TYPE_EVENT_DATA_BUFFER};
    while (xrPollEvent(instance, &ev) == XR_SUCCESS) {
      if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
        const auto *change =
            reinterpret_cast<const XrEventDataSessionStateChanged *>(&ev);
        if (change->state == XR_SESSION_STATE_READY) {
          XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO, nullptr,
                                   XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO};
          check(xrBeginSession(session, &begin));
          running = true;
        }
        if (change->state == XR_SESSION_STATE_STOPPING) {
          check(xrEndSession(session));
          running = false;
          exit = true;
        }
        if (change->state == XR_SESSION_STATE_EXITING ||
            change->state == XR_SESSION_STATE_LOSS_PENDING)
          exit = true;
      }
      ev.type = XR_TYPE_EVENT_DATA_BUFFER;
    }
    if (!running) {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
      continue;
    }
    XrFrameState frame{XR_TYPE_FRAME_STATE};
    XrFrameWaitInfo wait{XR_TYPE_FRAME_WAIT_INFO};
    check(xrWaitFrame(session, &wait, &frame));
    XrFrameBeginInfo begin{XR_TYPE_FRAME_BEGIN_INFO};
    check(xrBeginFrame(session, &begin));
    XrDataSnapshotCreateInfoMNDX info{XR_TYPE_DATA_SNAPSHOT_CREATE_INFO_MNDX,
                                      nullptr, base,
                                      frame.predictedDisplayTime};
    XrDataSnapshotMNDX data;
    check(acquire(session, &info, &data));
    uint32_t mesh_size = 0, video_size = 0;
    check(mesh(data, 0, &mesh_size, nullptr));
    check(video(data, 0, &video_size, nullptr));
    std::vector<uint8_t> video_bytes(video_size);
    if (video_size)
      check(video(data, video_size, &video_size, video_bytes.data()));
    // video_bytes: StereoVideoFrameHeaderV1, then left_size_bytes, then
    // right_size_bytes. Preserve stride/pixel_format from the header when
    // uploading to GPU textures.
    check(release(data));
    uint32_t entity_count = 0;
    if (context) {
      XrSpatialDiscoverySnapshotCreateInfoEXT discovery{
          XR_TYPE_SPATIAL_DISCOVERY_SNAPSHOT_CREATE_INFO_EXT};
      XrFutureEXT future;
      check(get<PFN_xrCreateSpatialDiscoverySnapshotAsyncEXT>(
          instance, "xrCreateSpatialDiscoverySnapshotAsyncEXT")(
          context, &discovery, &future));
      XrCreateSpatialDiscoverySnapshotCompletionInfoEXT complete{
          XR_TYPE_CREATE_SPATIAL_DISCOVERY_SNAPSHOT_COMPLETION_INFO_EXT,
          nullptr, base, frame.predictedDisplayTime, future};
      XrCreateSpatialDiscoverySnapshotCompletionEXT done{
          XR_TYPE_CREATE_SPATIAL_DISCOVERY_SNAPSHOT_COMPLETION_EXT};
      check(get<PFN_xrCreateSpatialDiscoverySnapshotCompleteEXT>(
          instance, "xrCreateSpatialDiscoverySnapshotCompleteEXT")(
          context, &complete, &done));
      check(done.futureResult);
      XrSpatialComponentDataQueryConditionEXT condition{
          XR_TYPE_SPATIAL_COMPONENT_DATA_QUERY_CONDITION_EXT};
      XrSpatialComponentDataQueryResultEXT result{
          XR_TYPE_SPATIAL_COMPONENT_DATA_QUERY_RESULT_EXT};
      check(get<PFN_xrQuerySpatialComponentDataEXT>(
          instance, "xrQuerySpatialComponentDataEXT")(done.snapshot, &condition,
                                                      &result));
      entity_count = result.entityIdCountOutput;
      check(get<PFN_xrDestroySpatialSnapshotEXT>(
          instance, "xrDestroySpatialSnapshotEXT")(done.snapshot));
    }
    std::cout << "mesh_bytes=" << mesh_size << " video_bytes=" << video_size
              << " entities=" << entity_count << '\n';
    XrFrameEndInfo finish{
        XR_TYPE_FRAME_END_INFO,           nullptr, frame.predictedDisplayTime,
        XR_ENVIRONMENT_BLEND_MODE_OPAQUE, 0,       nullptr};
    check(xrEndFrame(session, &finish));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  if (context)
    check(get<PFN_xrDestroySpatialContextEXT>(
        instance, "xrDestroySpatialContextEXT")(context));
  check(xrDestroySpace(base));
  check(xrDestroySession(session));
  check(xrDestroyInstance(instance));
  return 0;
} catch (const std::exception &e) {
  std::cerr << e.what() << '\n';
  return 1;
}
