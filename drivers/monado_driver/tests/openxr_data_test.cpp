// Production OpenXR entrypoints and sensor threads, with real SHM publishers.
// Only Monado's handle/space adapter is replaced, so no HMD/GPU is required.
#include "../../../runtime_adapters/xr_runtime_adapter/src/app_tcp_server.hpp"
#include "../openxr/spatial_api.cpp"
#include <cassert>
#include <chrono>
#include <iostream>
#include <thread>
#include <unistd.h>
#include <xr_runtime/publishers/runtime_pose_shm_publisher.hpp>
#include <xr_spatial/publishers/runtime_spatial_proxy_mesh_shm_publisher.hpp>
#include <xr_video/net/stereo_video_tcp.hpp>
#include <xr_video/shm/stereo_video_shm_publisher.hpp>
static XrInstance instance = reinterpret_cast<XrInstance>(uintptr_t{0x10000});
static XrSession xr_session = reinterpret_cast<XrSession>(uintptr_t{0x20000});
static XrSpace base = reinterpret_cast<XrSpace>(uintptr_t{0x30000});
extern "C" XrResult xr_gate_validate_instance(XrInstance i, const char *,
                                              XrSystemId sys) {
  return i != instance          ? XR_ERROR_HANDLE_INVALID
         : sys != 0 && sys != 1 ? XR_ERROR_SYSTEM_INVALID
                                : XR_SUCCESS;
}
extern "C" XrResult xr_gate_validate_session(XrSession s, const char *,
                                             XrInstance *i) {
  if (s != xr_session)
    return XR_ERROR_HANDLE_INVALID;
  *i = instance;
  return XR_SUCCESS;
}
extern "C" XrResult xr_gate_locate_runtime(XrSession s, XrSpace b, XrTime time,
                                           XrPosef *out) {
  if (s != xr_session || b != base)
    return XR_ERROR_HANDLE_INVALID;
  if (time <= 0)
    return XR_ERROR_TIME_INVALID;
  *out = {{0, 0, 0, 1}, {10, 20, 30}};
  return XR_SUCCESS;
}
template <class F> void eventually(F f, int line = __builtin_LINE()) {
  const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(4);
  while (std::chrono::steady_clock::now() < until) {
    if (f())
      return;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  std::cerr << "Timed out waiting for asynchronous producer/consumer, line "
            << line << "\n";
  std::abort();
}
int main() {
  using namespace xr::monado_driver;
  const auto prefix = "xrgate_data_test_" + std::to_string(getpid());
  const auto path = "/tmp/" + prefix + ".json";
  setenv("XR_MONADO_SPATIAL", "1", 1);
  setenv("XR_MONADO_VIDEO", "1", 1);
  setenv("XR_MONADO_HMD_SHM", (prefix + "_pose").c_str(), 1);
  setenv("XR_MONADO_SPATIAL_SHM", (prefix + "_mesh").c_str(), 1);
  setenv("XR_MONADO_VIDEO_SHM", (prefix + "_video").c_str(), 1);
  setenv("XR_MONADO_SPATIAL_MAX_AGE_MS", "150", 1);
  setenv("XR_MONADO_VIDEO_MAX_AGE_MS", "150", 1);
  xr_runtime::RuntimePoseShmPublisherConfig pc;
  pc.shm_name = prefix + "_pose";
  pc.registry_path = path;
  pc.unlink_existing = true;
  auto pub = std::make_unique<xr_runtime::RuntimePoseShmPublisher>(pc);
  xr_spatial::RuntimeSpatialProxyMeshShmPublisherConfig mc;
  mc.shm_name = prefix + "_mesh";
  mc.registry_path = path;
  mc.frame_id = "runtime_local";
  mc.unlink_existing = true;
  auto meshpub =
      std::make_unique<xr_spatial::RuntimeSpatialProxyMeshShmPublisher>(mc);
  xr_video::StereoVideoShmPublisherConfig vc;
  vc.shm_name = prefix + "_video";
  vc.registry_path = path;
  vc.width = 2;
  vc.height = 2;
  vc.unlink_existing = true;
  auto videopub = std::make_unique<xr_video::StereoVideoShmPublisher>(vc);
  xr_runtime::RuntimeHmdPoseF64V1 pose;
  pose.flags = xr_runtime::RUNTIME_HMD_FLAG_POSE_VALID;
  pose.tracking_status = 2;
  pose.confidence = 1;
  auto publish_pose = [&] {
    pose.timestamp_ns = monotonic_now_ns();
    pub->publish(pose);
  };
  xr_spatial::RuntimeSpatialProxyMeshF32V1 mesh;
  mesh.vertex_count = 4;
  mesh.triangle_count = 2;
  mesh.vertices[0] = {0, 0, -1};
  mesh.vertices[1] = {1, 0, -1};
  mesh.vertices[2] = {0, 1, -1};
  mesh.vertices[3] = {NAN, NAN, NAN};
  mesh.triangles[0] = {0, 1, 2};
  mesh.triangles[1] = {0, 1, 3};
  mesh.status_flags = xr_spatial::RUNTIME_SPATIAL_PROXY_MESH_FLAG_ACTIVE;
  auto publish_mesh = [&] {
    mesh.timestamp_ns = monotonic_now_ns();
    ++mesh.sequence;
    meshpub->publish(mesh);
  };
  xr_video::StereoVideoFrame video;
  video.header.width = video.header.height = 2;
  video.header.stride_left = video.header.stride_right = 2;
  video.left = {1, 2, 3, 4};
  video.right = {5, 6, 7, 8};
  video.header.left_size_bytes = video.header.right_size_bytes = 4;
  auto publish_video = [&] {
    video.header.timestamp_ns = monotonic_now_ns();
    videopub->publish(video);
  };
  assert(xr_gate_extension_available(entity_ext));
  setenv("XR_MONADO_SPATIAL", "0", 1);
  assert(!xr_gate_extension_available(entity_ext));
  setenv("XR_MONADO_SPATIAL", "1", 1);
  assert(!xr_gate_reacquired(xr_session));
  publish_pose();
  eventually([&] {
    publish_pose();
    return xr_gate_reacquired(xr_session);
  });
  assert(!xr_gate_reacquired(xr_session));
  // A changed reset_counter with continuously valid tracking is NOT a reset.
  ++pose.reset_counter;
  publish_pose();
  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  assert(!xr_gate_reacquired(xr_session));
  publish_mesh();
  publish_video();
  eventually([&] {
    publish_pose();
    publish_mesh();
    publish_video();
    auto s = sessions.at(xr_session).source->latest();
    return s.mesh_world_valid && bool(s.video);
  });
  XrSpatialCapabilityEXT caps[1];
  uint32_t n = 0;
  assert(enumerate_capabilities(instance, 1, 1, &n, caps) == XR_SUCCESS &&
         n == 1);
  const XrSpatialComponentTypeEXT types[] = {
      XR_SPATIAL_COMPONENT_TYPE_BOUNDED_3D_EXT,
      XR_SPATIAL_COMPONENT_TYPE_MESH_3D_EXT};
  XrSpatialCapabilityConfigurationMeshTrackingEXT cfg{
      XR_TYPE_SPATIAL_CAPABILITY_CONFIGURATION_MESH_TRACKING_EXT, nullptr,
      XR_SPATIAL_CAPABILITY_MESH_TRACKING_EXT, 2, types};
  const auto *config =
      reinterpret_cast<const XrSpatialCapabilityConfigurationBaseHeaderEXT *>(
          &cfg);
  XrSpatialContextCreateInfoEXT ci{XR_TYPE_SPATIAL_CONTEXT_CREATE_INFO_EXT,
                                   nullptr, 1, &config};
  XrFutureEXT future;
  assert(create_context(xr_session, &ci, &future) == XR_SUCCESS);
  XrFuturePollInfoEXT pi{XR_TYPE_FUTURE_POLL_INFO_EXT, nullptr, future};
  XrFuturePollResultEXT pr{XR_TYPE_FUTURE_POLL_RESULT_EXT};
  assert(xr_gate_owns_future(future) &&
         xr_gate_poll_future(instance, &pi, &pr) == XR_SUCCESS &&
         pr.state == XR_FUTURE_STATE_READY_EXT);
  XrCreateSpatialContextCompletionEXT cc{
      XR_TYPE_CREATE_SPATIAL_CONTEXT_COMPLETION_EXT};
  assert(complete_context(xr_session, future, &cc) == XR_SUCCESS);
  assert(xr_gate_poll_future(instance, &pi, &pr) ==
         XR_ERROR_FUTURE_INVALID_EXT);
  auto context = cc.spatialContext;
  XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
  assert(xr_gate_poll_event(instance, &event));
  assert(!xr_gate_poll_event(instance, &event));
  XrSpatialDiscoverySnapshotCreateInfoEXT di{
      XR_TYPE_SPATIAL_DISCOVERY_SNAPSHOT_CREATE_INFO_EXT};
  assert(create_discovery(context, &di, &future) == XR_SUCCESS);
  XrCreateSpatialDiscoverySnapshotCompletionInfoEXT done{
      XR_TYPE_CREATE_SPATIAL_DISCOVERY_SNAPSHOT_COMPLETION_INFO_EXT, nullptr,
      base, 1, future};
  XrCreateSpatialDiscoverySnapshotCompletionEXT dc{
      XR_TYPE_CREATE_SPATIAL_DISCOVERY_SNAPSHOT_COMPLETION_EXT};
  assert(complete_discovery(context, &done, &dc) == XR_SUCCESS);
  auto snapshot = dc.snapshot;
  XrSpatialComponentDataQueryConditionEXT qi{
      XR_TYPE_SPATIAL_COMPONENT_DATA_QUERY_CONDITION_EXT};
  XrSpatialComponentDataQueryResultEXT qr{
      XR_TYPE_SPATIAL_COMPONENT_DATA_QUERY_RESULT_EXT};
  assert(query(snapshot, &qi, &qr) == XR_SUCCESS &&
         qr.entityIdCountOutput == 1);
  XrSpatialMeshDataEXT meshdata{};
  XrBoxf bounds{};
  XrSpatialComponentBounded3DListEXT bl{
      XR_TYPE_SPATIAL_COMPONENT_BOUNDED_3D_LIST_EXT, nullptr, 1, &bounds};
  XrSpatialComponentMesh3DListEXT ml{XR_TYPE_SPATIAL_COMPONENT_MESH_3D_LIST_EXT,
                                     &bl, 1, &meshdata};
  XrSpatialEntityIdEXT entity_id;
  qr.next = &ml;
  qr.entityIdCapacityInput = 1;
  qr.entityIds = &entity_id;
  assert(query(snapshot, &qi, &qr) == XR_SUCCESS);
  assert(meshdata.origin.position.x == 10 && bounds.center.position.x == 10.5f);
  XrSpatialBufferGetInfoEXT bi{XR_TYPE_SPATIAL_BUFFER_GET_INFO_EXT, nullptr, 1};
  assert(get_vertices(snapshot, &bi, 0, &n, nullptr) == XR_SUCCESS && n == 3);
  bi.bufferId = 2;
  std::vector<uint32_t> indices(3);
  assert(get_indices(snapshot, &bi, 3, &n, indices.data()) == XR_SUCCESS &&
         n == 3 && indices[2] == 2);
  assert(get_indices(snapshot, &bi, 2, &n, indices.data()) ==
         XR_ERROR_SIZE_INSUFFICIENT);
  // Raw getters retain every byte, including NaNs and rejected triangles.
  XrDataSnapshotCreateInfoMNDX rawinfo{XR_TYPE_DATA_SNAPSHOT_CREATE_INFO_MNDX,
                                       nullptr, base, 1};
  XrDataSnapshotMNDX raw;
  assert(create_raw(xr_session, &rawinfo, &raw) == XR_SUCCESS);
  assert(raw_mesh(raw, 0, &n, nullptr) == XR_SUCCESS && n == sizeof(mesh));
  std::vector<uint8_t> rawbytes(n);
  assert(raw_mesh(raw, n, &n, rawbytes.data()) == XR_SUCCESS);
  xr_spatial::RuntimeSpatialProxyMeshF32V1 rawcopy;
  std::memcpy(&rawcopy, rawbytes.data(), rawbytes.size());
  assert(rawcopy.vertex_count == 4 && std::isnan(rawcopy.vertices[3].x));
  assert(raw_video(raw, 0, &n, nullptr) == XR_SUCCESS && n == 104);
  rawbytes.resize(n);
  assert(raw_video(raw, n, &n, rawbytes.data()) == XR_SUCCESS &&
         rawbytes[96] == 1 && rawbytes[100] == 5);
  auto expected_vertices = snapshots.at(snapshot).vertices;
  mesh.vertices[1].x = 2;
  publish_mesh();
  std::this_thread::sleep_for(std::chrono::milliseconds(25));
  assert(snapshots.at(snapshot).vertices[1].x == expected_vertices[1].x);
  XrSpatialEntityFromIdCreateInfoEXT ei{
      XR_TYPE_SPATIAL_ENTITY_FROM_ID_CREATE_INFO_EXT, nullptr, entity_id};
  XrSpatialEntityEXT entity;
  assert(create_entity(context, &ei, &entity) == XR_SUCCESS);
  assert(destroy_snapshot(snapshot) == XR_SUCCESS);
  XrSpatialEntityEXT second_entity;
  assert(create_entity(context, &ei, &second_entity) == XR_SUCCESS);
  // Expiry does not refresh timestamps just because the reader copies a sample
  // again.
  std::this_thread::sleep_for(std::chrono::milliseconds(190));
  assert(!sessions.at(xr_session).source->latest().mesh);
  pose.flags = 0;
  publish_pose();
  std::this_thread::sleep_for(std::chrono::milliseconds(35));
  assert(!xr_gate_reacquired(xr_session));
  pose.flags = xr_runtime::RUNTIME_HMD_FLAG_POSE_VALID;
  publish_pose();
  eventually([&] {
    publish_pose();
    return xr_gate_reacquired(xr_session);
  });
  assert(!xr_gate_reacquired(xr_session));
  assert(!sessions.at(xr_session).source->latest().mesh_world_valid);
  publish_mesh();
  eventually([&] {
    publish_pose();
    publish_mesh();
    return sessions.at(xr_session).source->latest().mesh_world_valid;
  });
  XrSpatialUpdateSnapshotCreateInfoEXT ui{
      XR_TYPE_SPATIAL_UPDATE_SNAPSHOT_CREATE_INFO_EXT,
      nullptr,
      1,
      &entity,
      0,
      nullptr,
      base,
      1};
  XrSpatialSnapshotEXT updated;
  assert(create_update(context, &ui, &updated) == XR_SUCCESS);
  assert(snapshots.at(updated).state ==
         XR_SPATIAL_ENTITY_TRACKING_STATE_STOPPED_EXT);
  // A single update may include a stopped entity and the current live entity.
  assert(create_discovery(context, &di, &future) == XR_SUCCESS);
  done.future = future;
  assert(complete_discovery(context, &done, &dc) == XR_SUCCESS);
  qr.next = nullptr;
  qr.entityIdCapacityInput = 1;
  qr.entityIds = &entity_id;
  assert(query(dc.snapshot, &qi, &qr) == XR_SUCCESS &&
         qr.entityIdCountOutput == 1);
  ei.entityId = entity_id;
  XrSpatialEntityEXT live_entity;
  assert(create_entity(context, &ei, &live_entity) == XR_SUCCESS);
  XrSpatialEntityEXT both[] = {entity, live_entity};
  ui.entityCount = 2;
  ui.entities = both;
  assert(create_update(context, &ui, &updated) == XR_SUCCESS);
  qr.entityIdCapacityInput = 0;
  assert(query(updated, &qi, &qr) == XR_SUCCESS && qr.entityIdCountOutput == 2);
  XrSpatialFilterTrackingStateEXT tracked{
      XR_TYPE_SPATIAL_FILTER_TRACKING_STATE_EXT, nullptr,
      XR_SPATIAL_ENTITY_TRACKING_STATE_TRACKING_EXT};
  qi.next = &tracked;
  assert(query(updated, &qi, &qr) == XR_SUCCESS && qr.entityIdCountOutput == 1);
  qi.next = nullptr;
  xr_gate_destroy_session(xr_session);
  assert(contexts.empty() && snapshots.empty() && futures.empty() &&
         raw_snapshots.empty());
  // Exercise the same application TCP server and client used by SteamVR,
  // including mesh delivery and recovery when both the server and video
  // restart.
  auto free_port = [] {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    assert(fd >= 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(::bind(fd, reinterpret_cast<sockaddr *>(&address),
                  sizeof(address)) == 0);
    socklen_t length = sizeof(address);
    assert(::getsockname(fd, reinterpret_cast<sockaddr *>(&address), &length) ==
           0);
    const auto port = ntohs(address.sin_port);
    ::close(fd);
    return port;
  };
  xr_runtime_adapter::AppTcpServerConfig tcp_config;
  tcp_config.port = free_port();
  tcp_config.spatial_mesh = true;
  tcp_config.rate_hz = 120;
  auto server = std::make_unique<xr_runtime_adapter::AppTcpServer>(tcp_config);
  setenv("XR_MONADO_TRANSPORT", "tcp", 1);
  setenv("XR_MONADO_TCP_PORT", std::to_string(tcp_config.port).c_str(), 1);
  setenv("XR_MONADO_TCP_RECONNECT_MS", "40", 1);
  setenv("XR_MONADO_TCP_TIMEOUT_MS", "400", 1);
  const auto video_port = free_port();
  setenv("XR_MONADO_VIDEO_TRANSPORT", "tcp", 1);
  setenv("XR_MONADO_VIDEO_TCP_PORT", std::to_string(video_port).c_str(), 1);
  auto video_server =
      std::make_unique<xr_video::StereoVideoTcpServer>("127.0.0.1", video_port);
  xr_runtime_adapter::AppTcpFrame frame;
  auto send = [&] {
    frame.read_timestamp_ns = monotonic_now_ns() + 9007199254740993LL;
    pose.timestamp_ns = frame.read_timestamp_ns;
    frame.hmd = pose;
    xr_tracking::HandTrackingFrameF32V2 remote_hands;
    remote_hands.timestamp_ns = frame.read_timestamp_ns;
    remote_hands.source_timestamp_ns = frame.read_timestamp_ns - 50000000;
    frame.hands = remote_hands;
    xr_runtime::RuntimeControllerStateFrameV1 remote_inputs;
    remote_inputs.timestamp_ns = frame.read_timestamp_ns;
    frame.controllers = remote_inputs;
    mesh.timestamp_ns = frame.read_timestamp_ns;
    ++mesh.sequence;
    frame.spatial =
        std::make_shared<const xr_spatial::RuntimeSpatialProxyMeshF32V1>(mesh);
    server->publish(frame);
    ++video.header.sequence;
    video_server->publish(video);
  };
  assert(!xr_gate_reacquired(xr_session));
  eventually([&] {
    send();
    return xr_gate_reacquired(xr_session);
  });
  eventually([&] {
    send();
    auto s = sessions.at(xr_session).source->latest();
    return s.mesh_world_valid && bool(s.video);
  });
  auto tcp_pose_cfg = RuntimePoseReaderConfig{};
  tcp_pose_cfg.transport = "tcp";
  tcp_pose_cfg.tcp.port = tcp_config.port;
  tcp_pose_cfg.tcp.reconnect_ms = 40;
  tcp_pose_cfg.tcp.timeout_ms = 400;
  auto tcp_reader = create_runtime_pose_reader(tcp_pose_cfg);
  RuntimePoseSample tcp_sample;
  eventually([&] {
    send();
    return tcp_reader->read_latest(tcp_sample, nullptr);
  });
  assert(runtime_pose_is_valid(tcp_sample.pose) &&
         tcp_sample.age_ns < 100000000);
  // All readers for the endpoint share the worker; releasing one leaves others
  // live.
  RuntimeHandReaderConfig tcp_hand_config;
  tcp_hand_config.transport = "tcp";
  tcp_hand_config.tcp = tcp_pose_cfg.tcp;
  auto tcp_hand_reader = create_runtime_hand_reader(tcp_hand_config);
  RuntimeHandSample tcp_hand;
  eventually([&] {
    send();
    return tcp_hand_reader->read_latest(tcp_hand, nullptr);
  });
  const auto optical_age =
      tcp_hand.read_time_ns - int64_t(tcp_hand.frame.source_timestamp_ns);
  assert(optical_age >= 50000000 && optical_age < 150000000);
  RuntimeControllerStateReaderConfig tcp_input_config;
  tcp_input_config.transport = "tcp";
  tcp_input_config.tcp = tcp_pose_cfg.tcp;
  auto tcp_input_reader =
      create_runtime_controller_state_reader(tcp_input_config);
  RuntimeControllerStateSample tcp_input;
  eventually([&] {
    send();
    return tcp_input_reader->read_latest(tcp_input, nullptr);
  });
  assert(std::abs(tcp_input.read_time_ns -
                  int64_t(tcp_input.frame.timestamp_ns)) < 100000000);
  tcp_hand_reader.reset();
  tcp_input_reader.reset();
  tcp_reader.reset();
  server.reset();
  video_server.reset();
  eventually(
      [&] { return !sessions.at(xr_session).source->latest().tracking_valid; });
  assert(!xr_gate_reacquired(xr_session));
  server = std::make_unique<xr_runtime_adapter::AppTcpServer>(tcp_config);
  video_server =
      std::make_unique<xr_video::StereoVideoTcpServer>("127.0.0.1", video_port);
  eventually([&] {
    send();
    return xr_gate_reacquired(xr_session);
  });
  eventually([&] {
    send();
    auto s = sessions.at(xr_session).source->latest();
    return s.mesh_world_valid && bool(s.video);
  });
  xr_gate_destroy_session(xr_session);
  server.reset();
  video_server.reset();
  unsetenv("XR_MONADO_TRANSPORT");
  unsetenv("XR_MONADO_VIDEO_TRANSPORT");
  for (auto suffix : {"_pose", "_mesh", "_video"})
    shm_unlink(("/" + prefix + suffix).c_str());
  unlink(path.c_str());
  std::cout << "OpenXR data: entities, futures, transforms, exact raw frames, "
               "immutable snapshots, loss/reacquire, stale data and teardown "
               "passed\n";
}
