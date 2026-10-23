#include "oxr_bridge.h"
#include "sensor_source.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <vector>
using xr::monado_driver::SensorSource;
using xr::monado_driver::SensorState;
namespace {
std::recursive_mutex mutex;
constexpr const char *entity_ext = XR_EXT_SPATIAL_ENTITY_EXTENSION_NAME;
constexpr const char *mesh_ext = XR_EXT_SPATIAL_MESH_TRACKING_EXTENSION_NAME;
constexpr const char *raw_ext = XR_MNDX_XR_GATE_DATA_EXTENSION_NAME;
constexpr uint64_t future_tag = 0x5852470000000000ULL;
constexpr size_t handle_limit = 1024;
uint64_t next_id = 1;
uint64_t id() { return next_id++; }
template <class H> H handle(uint64_t value) {
  return reinterpret_cast<H>(static_cast<uintptr_t>(value));
}
struct Session {
  XrInstance instance;
  std::shared_ptr<SensorSource> source;
  uint64_t recentered_epoch = 0;
};
struct Context {
  XrSession session;
  std::set<XrSpatialComponentTypeEXT> components;
  uint64_t discovery_sequence = 0, discovery_epoch = 0;
  bool event_sent = false;
  std::set<XrSpatialEntityIdEXT> known_ids;
};
struct Snapshot {
  XrSpatialContextEXT context;
  XrSession session;
  std::set<XrSpatialComponentTypeEXT> components;
  std::vector<XrSpatialEntityIdEXT> stopped_entities;
  std::vector<XrSpatialEntityIdEXT> paused_entities;
  XrSpatialEntityIdEXT entity = 0;
  XrSpatialEntityTrackingStateEXT state =
      XR_SPATIAL_ENTITY_TRACKING_STATE_STOPPED_EXT;
  std::vector<XrVector3f> vertices;
  std::vector<uint32_t> indices;
  XrBoxf bounds{};
  XrPosef origin{{0, 0, 0, 1}, {0, 0, 0}};
};
struct Entity {
  XrSpatialContextEXT context;
  XrSpatialEntityIdEXT id;
};
struct Future {
  XrSession session;
  XrSpatialContextEXT context = XR_NULL_HANDLE;
  std::set<XrSpatialComponentTypeEXT> components;
  SensorState sensor;
  XrSpatialEntityTrackingStateEXT tracking_filter =
      XR_SPATIAL_ENTITY_TRACKING_STATE_MAX_ENUM_EXT;
};
struct Raw {
  XrSession session;
  SensorState sensor;
  XrPosef transform;
};
std::map<XrSession, Session> sessions;
std::map<XrSpatialContextEXT, Context> contexts;
std::map<XrSpatialSnapshotEXT, Snapshot> snapshots;
std::map<XrSpatialEntityEXT, Entity> entities;
std::map<XrFutureEXT, Future> futures;
std::map<XrDataSnapshotMNDX, Raw> raw_snapshots;
std::weak_ptr<SensorSource> common_source;
Session &session(XrSession h, XrInstance instance) {
  auto it = sessions.find(h);
  if (it != sessions.end())
    return it->second;
  auto source = common_source.lock();
  if (!source) {
    source = std::make_shared<SensorSource>();
    common_source = source;
  }
  return sessions.emplace(h, Session{instance, source}).first->second;
}
#define LOCK std::lock_guard<std::recursive_mutex> guard(mutex)
#define API_END                                                                \
  catch (const std::bad_alloc &) {                                             \
    return XR_ERROR_OUT_OF_MEMORY;                                             \
  }                                                                            \
  catch (...) {                                                                \
    return XR_ERROR_RUNTIME_FAILURE;                                           \
  }
#define TYPE(p, t)                                                             \
  if (!(p) || (p)->type != (t))                                                \
  return XR_ERROR_VALIDATION_FAILURE
#define CHECK(call)                                                            \
  do {                                                                         \
    const XrResult r = (call);                                                 \
    if (XR_FAILED(r))                                                          \
      return r;                                                                \
  } while (0)
template <class T>
XrResult copy(uint32_t capacity, uint32_t *count, T *output,
              const std::vector<T> &data) {
  if (!count)
    return XR_ERROR_VALIDATION_FAILURE;
  *count = uint32_t(data.size());
  if (capacity == 0)
    return XR_SUCCESS;
  if (!output)
    return XR_ERROR_VALIDATION_FAILURE;
  if (capacity < data.size())
    return XR_ERROR_SIZE_INSUFFICIENT;
  std::copy(data.begin(), data.end(), output);
  return XR_SUCCESS;
}
XrResult components(uint32_t count, const XrSpatialComponentTypeEXT *input,
                    const std::set<XrSpatialComponentTypeEXT> &enabled,
                    std::set<XrSpatialComponentTypeEXT> &out) {
  if (!count) {
    out = enabled;
    return XR_SUCCESS;
  }
  if (!input || count > 32)
    return XR_ERROR_VALIDATION_FAILURE;
  for (uint32_t i = 0; i < count; ++i) {
    if (!enabled.count(input[i]))
      return XR_ERROR_SPATIAL_COMPONENT_NOT_ENABLED_EXT;
    if (!out.insert(input[i]).second)
      return XR_ERROR_VALIDATION_FAILURE;
  }
  return XR_SUCCESS;
}
XrVector3f rotate(const XrQuaternionf &q, XrVector3f v) {
  const XrVector3f t{2 * (q.y * v.z - q.z * v.y), 2 * (q.z * v.x - q.x * v.z),
                     2 * (q.x * v.y - q.y * v.x)};
  return {v.x + q.w * t.x + q.y * t.z - q.z * t.y,
          v.y + q.w * t.y + q.z * t.x - q.x * t.z,
          v.z + q.w * t.z + q.x * t.y - q.y * t.x};
}
bool finite(XrVector3f v) {
  return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}
Snapshot geometry(XrSpatialContextEXT context, const Future &f,
                  const SensorState &current, const XrPosef &transform) {
  Snapshot out{};
  out.context = context;
  out.session = f.session;
  out.components = f.components;
  // A delayed async completion cannot revive geometry from a previous world.
  if (!f.sensor.mesh || !f.sensor.mesh_world_valid || !current.tracking_valid ||
      f.sensor.tracking_epoch != current.tracking_epoch)
    return out;
  if (f.tracking_filter != XR_SPATIAL_ENTITY_TRACKING_STATE_MAX_ENUM_EXT &&
      f.tracking_filter != XR_SPATIAL_ENTITY_TRACKING_STATE_TRACKING_EXT)
    return out;
  const auto &m = *f.sensor.mesh;
  if (!(m.status_flags & xr_spatial::RUNTIME_SPATIAL_PROXY_MESH_FLAG_ACTIVE) ||
      (m.status_flags &
       xr_spatial::RUNTIME_SPATIAL_PROXY_MESH_FLAG_CLEAR_FRAME) ||
      !m.triangle_count)
    return out;
  out.entity = 0x5852000000000000ULL + f.sensor.tracking_epoch;
  out.state = XR_SPATIAL_ENTITY_TRACKING_STATE_TRACKING_EXT;
  std::vector<uint32_t> remap(m.vertex_count, UINT32_MAX);
  for (uint32_t i = 0; i < m.vertex_count; ++i) {
    const auto &v = m.vertices[i];
    XrVector3f p{v.x, v.y, v.z};
    if (!finite(p))
      continue;
    remap[i] = uint32_t(out.vertices.size());
    out.vertices.push_back(p);
  }
  for (uint32_t i = 0; i < m.triangle_count; ++i) {
    const auto &t = m.triangles[i];
    if (t.i0 >= remap.size() || t.i1 >= remap.size() || t.i2 >= remap.size())
      continue;
    const auto a = remap[t.i0], b = remap[t.i1], c = remap[t.i2];
    if (a == UINT32_MAX || b == UINT32_MAX || c == UINT32_MAX || a == b ||
        a == c || b == c)
      continue;
    const auto p = out.vertices[a], q = out.vertices[b], r = out.vertices[c];
    const XrVector3f u{q.x - p.x, q.y - p.y, q.z - p.z},
        v{r.x - p.x, r.y - p.y, r.z - p.z};
    const XrVector3f n{u.y * v.z - u.z * v.y, u.z * v.x - u.x * v.z,
                       u.x * v.y - u.y * v.x};
    const double area =
        double(n.x) * n.x + double(n.y) * n.y + double(n.z) * n.z;
    if (!std::isfinite(area) || area < 1e-16)
      continue;
    out.indices.insert(out.indices.end(), {a, b, c});
  }
  if (out.indices.empty()) {
    out.entity = 0;
    out.vertices.clear();
    return out;
  }
  auto low = out.vertices[0], high = low;
  for (auto p : out.vertices) {
    low.x = std::min(low.x, p.x);
    low.y = std::min(low.y, p.y);
    low.z = std::min(low.z, p.z);
    high.x = std::max(high.x, p.x);
    high.y = std::max(high.y, p.y);
    high.z = std::max(high.z, p.z);
  }
  out.origin = transform;
  auto center = rotate(
      transform.orientation,
      {(low.x + high.x) * .5f, (low.y + high.y) * .5f, (low.z + high.z) * .5f});
  out.bounds.center = {transform.orientation,
                       {center.x + transform.position.x,
                        center.y + transform.position.y,
                        center.z + transform.position.z}};
  out.bounds.extents = {high.x - low.x, high.y - low.y, high.z - low.z};
  return out;
}
template <class M> void erase_session(M &map, XrSession h) {
  for (auto i = map.begin(); i != map.end();)
    if (i->second.session == h)
      i = map.erase(i);
    else
      ++i;
}
XrResult XRAPI_CALL enumerate_capabilities(XrInstance inst, XrSystemId sys,
                                           uint32_t cap, uint32_t *count,
                                           XrSpatialCapabilityEXT *out) try {
  CHECK(xr_gate_validate_instance(inst, entity_ext, sys));
  std::vector<XrSpatialCapabilityEXT> data;
  if (XR_SUCCEEDED(xr_gate_validate_instance(inst, mesh_ext, sys)))
    data.push_back(XR_SPATIAL_CAPABILITY_MESH_TRACKING_EXT);
  return copy(cap, count, out, data);
}
API_END
XrResult XRAPI_CALL enumerate_components(
    XrInstance inst, XrSystemId sys, XrSpatialCapabilityEXT capability,
    XrSpatialCapabilityComponentTypesEXT *out) try {
  CHECK(xr_gate_validate_instance(inst, entity_ext, sys));
  TYPE(out, XR_TYPE_SPATIAL_CAPABILITY_COMPONENT_TYPES_EXT);
  if (capability != XR_SPATIAL_CAPABILITY_MESH_TRACKING_EXT ||
      XR_FAILED(xr_gate_validate_instance(inst, mesh_ext, sys)))
    return XR_ERROR_SPATIAL_CAPABILITY_UNSUPPORTED_EXT;
  return copy(out->componentTypeCapacityInput, &out->componentTypeCountOutput,
              out->componentTypes,
              std::vector<XrSpatialComponentTypeEXT>{
                  XR_SPATIAL_COMPONENT_TYPE_BOUNDED_3D_EXT,
                  XR_SPATIAL_COMPONENT_TYPE_MESH_3D_EXT});
}
API_END
XrResult XRAPI_CALL enumerate_features(XrInstance inst, XrSystemId sys,
                                       XrSpatialCapabilityEXT capability,
                                       uint32_t cap, uint32_t *count,
                                       XrSpatialCapabilityFeatureEXT *out) try {
  CHECK(xr_gate_validate_instance(inst, entity_ext, sys));
  if (capability != XR_SPATIAL_CAPABILITY_MESH_TRACKING_EXT ||
      XR_FAILED(xr_gate_validate_instance(inst, mesh_ext, sys)))
    return XR_ERROR_SPATIAL_CAPABILITY_UNSUPPORTED_EXT;
  return copy(cap, count, out, std::vector<XrSpatialCapabilityFeatureEXT>{});
}
API_END
XrResult XRAPI_CALL create_context(XrSession sess,
                                   const XrSpatialContextCreateInfoEXT *info,
                                   XrFutureEXT *future) try {
  XrInstance inst;
  CHECK(xr_gate_validate_session(sess, entity_ext, &inst));
  CHECK(xr_gate_validate_instance(inst, mesh_ext, XR_NULL_SYSTEM_ID));
  TYPE(info, XR_TYPE_SPATIAL_CONTEXT_CREATE_INFO_EXT);
  if (!future || !info->capabilityConfigs || info->capabilityConfigCount != 1)
    return XR_ERROR_SPATIAL_CAPABILITY_CONFIGURATION_INVALID_EXT;
  const auto *c = info->capabilityConfigs[0];
  if (!c || c->capability != XR_SPATIAL_CAPABILITY_MESH_TRACKING_EXT)
    return XR_ERROR_SPATIAL_CAPABILITY_UNSUPPORTED_EXT;
  if (c->type != XR_TYPE_SPATIAL_CAPABILITY_CONFIGURATION_MESH_TRACKING_EXT ||
      !c->enabledComponentCount || !c->enabledComponents ||
      c->enabledComponentCount > 2)
    return XR_ERROR_SPATIAL_CAPABILITY_CONFIGURATION_INVALID_EXT;
  Future f{};
  f.session = sess;
  for (uint32_t i = 0; i < c->enabledComponentCount; ++i) {
    auto type = c->enabledComponents[i];
    if (type != XR_SPATIAL_COMPONENT_TYPE_BOUNDED_3D_EXT &&
        type != XR_SPATIAL_COMPONENT_TYPE_MESH_3D_EXT)
      return XR_ERROR_SPATIAL_COMPONENT_UNSUPPORTED_FOR_CAPABILITY_EXT;
    if (!f.components.insert(type).second)
      return XR_ERROR_SPATIAL_CAPABILITY_CONFIGURATION_INVALID_EXT;
  }
  for (auto *n = static_cast<const XrBaseInStructure *>(c->next); n;
       n = n->next) {
    if (n->type == XR_TYPE_SPATIAL_FEATURE_TRACK_WATERTIGHT_MESH_EXT &&
        reinterpret_cast<const XrSpatialFeatureTrackWatertightMeshEXT *>(n)
            ->enableWatertightMeshTracking)
      return XR_ERROR_SPATIAL_CAPABILITY_CONFIGURATION_INVALID_EXT;
  }
  LOCK;
  if (futures.size() >= handle_limit || contexts.size() >= handle_limit)
    return XR_ERROR_LIMIT_REACHED;
  session(sess, inst);
  *future = handle<XrFutureEXT>(future_tag | id());
  futures.emplace(*future, std::move(f));
  return XR_SUCCESS;
}
API_END
XrResult XRAPI_CALL
complete_context(XrSession sess, XrFutureEXT future,
                 XrCreateSpatialContextCompletionEXT *out) try {
  XrInstance inst;
  CHECK(xr_gate_validate_session(sess, entity_ext, &inst));
  TYPE(out, XR_TYPE_CREATE_SPATIAL_CONTEXT_COMPLETION_EXT);
  LOCK;
  auto it = futures.find(future);
  if (it == futures.end() || it->second.session != sess || it->second.context)
    return XR_ERROR_FUTURE_INVALID_EXT;
  if (contexts.size() >= handle_limit)
    return XR_ERROR_LIMIT_REACHED;
  const auto h = handle<XrSpatialContextEXT>(id());
  contexts.emplace(h, Context{sess, it->second.components, 0, 0, false, {}});
  futures.erase(it);
  out->futureResult = XR_SUCCESS;
  out->spatialContext = h;
  return XR_SUCCESS;
}
API_END
XrResult XRAPI_CALL destroy_context(XrSpatialContextEXT h) try {
  LOCK;
  if (!contexts.count(h))
    return XR_ERROR_HANDLE_INVALID;
  for (auto i = snapshots.begin(); i != snapshots.end();)
    if (i->second.context == h)
      i = snapshots.erase(i);
    else
      ++i;
  for (auto i = entities.begin(); i != entities.end();)
    if (i->second.context == h)
      i = entities.erase(i);
    else
      ++i;
  for (auto i = futures.begin(); i != futures.end();)
    if (i->second.context == h)
      i = futures.erase(i);
    else
      ++i;
  contexts.erase(h);
  return XR_SUCCESS;
}
API_END
XrResult XRAPI_CALL create_discovery(
    XrSpatialContextEXT h, const XrSpatialDiscoverySnapshotCreateInfoEXT *info,
    XrFutureEXT *out) try {
  TYPE(info, XR_TYPE_SPATIAL_DISCOVERY_SNAPSHOT_CREATE_INFO_EXT);
  if (!out)
    return XR_ERROR_VALIDATION_FAILURE;
  LOCK;
  auto c = contexts.find(h);
  if (c == contexts.end())
    return XR_ERROR_HANDLE_INVALID;
  if (futures.size() >= handle_limit)
    return XR_ERROR_LIMIT_REACHED;
  Future f{};
  f.context = h;
  f.session = c->second.session;
  CHECK(components(info->componentTypeCount, info->componentTypes,
                   c->second.components, f.components));
  for (auto *n = static_cast<const XrBaseInStructure *>(info->next); n;
       n = n->next) {
    if (n->type == XR_TYPE_SPATIAL_FILTER_TRACKING_STATE_EXT) {
      const auto filter =
          reinterpret_cast<const XrSpatialFilterTrackingStateEXT *>(n)
              ->trackingState;
      if (filter != XR_SPATIAL_ENTITY_TRACKING_STATE_TRACKING_EXT &&
          filter != XR_SPATIAL_ENTITY_TRACKING_STATE_PAUSED_EXT &&
          filter != XR_SPATIAL_ENTITY_TRACKING_STATE_STOPPED_EXT)
        return XR_ERROR_VALIDATION_FAILURE;
      f.tracking_filter = filter;
    }
  }
  f.sensor = sessions.at(f.session).source->latest();
  c->second.discovery_sequence = f.sensor.mesh ? f.sensor.mesh->sequence : 0;
  c->second.discovery_epoch = f.sensor.tracking_epoch;
  c->second.event_sent = false;
  *out = handle<XrFutureEXT>(future_tag | id());
  futures.emplace(*out, std::move(f));
  return XR_SUCCESS;
}
API_END
XrResult XRAPI_CALL complete_discovery(
    XrSpatialContextEXT h,
    const XrCreateSpatialDiscoverySnapshotCompletionInfoEXT *info,
    XrCreateSpatialDiscoverySnapshotCompletionEXT *out) try {
  TYPE(info, XR_TYPE_CREATE_SPATIAL_DISCOVERY_SNAPSHOT_COMPLETION_INFO_EXT);
  TYPE(out, XR_TYPE_CREATE_SPATIAL_DISCOVERY_SNAPSHOT_COMPLETION_EXT);
  LOCK;
  auto c = contexts.find(h);
  if (c == contexts.end())
    return XR_ERROR_HANDLE_INVALID;
  auto f = futures.find(info->future);
  if (f == futures.end() || f->second.context != h)
    return XR_ERROR_FUTURE_INVALID_EXT;
  if (snapshots.size() >= handle_limit)
    return XR_ERROR_LIMIT_REACHED;
  XrPosef transform;
  CHECK(xr_gate_locate_runtime(c->second.session, info->baseSpace, info->time,
                               &transform));
  auto mesh = geometry(
      h, f->second, sessions.at(f->second.session).source->latest(), transform);
  if (mesh.entity)
    c->second.known_ids.insert(mesh.entity);
  const auto snapshot = handle<XrSpatialSnapshotEXT>(id());
  snapshots.emplace(snapshot, std::move(mesh));
  futures.erase(f);
  out->futureResult = XR_SUCCESS;
  out->snapshot = snapshot;
  return XR_SUCCESS;
}
API_END
XrResult XRAPI_CALL query(XrSpatialSnapshotEXT h,
                          const XrSpatialComponentDataQueryConditionEXT *info,
                          XrSpatialComponentDataQueryResultEXT *out) try {
  TYPE(info, XR_TYPE_SPATIAL_COMPONENT_DATA_QUERY_CONDITION_EXT);
  TYPE(out, XR_TYPE_SPATIAL_COMPONENT_DATA_QUERY_RESULT_EXT);
  LOCK;
  auto it = snapshots.find(h);
  if (it == snapshots.end())
    return XR_ERROR_HANDLE_INVALID;
  const auto &s = it->second;
  std::set<XrSpatialComponentTypeEXT> requested;
  CHECK(components(info->componentTypeCount, info->componentTypes, s.components,
                   requested));
  XrSpatialEntityTrackingStateEXT state_filter =
      XR_SPATIAL_ENTITY_TRACKING_STATE_MAX_ENUM_EXT;
  bool archetype_matches = true;
  for (auto *n = static_cast<const XrBaseInStructure *>(info->next); n;
       n = n->next) {
    if (n->type == XR_TYPE_SPATIAL_FILTER_TRACKING_STATE_EXT)
      state_filter =
          reinterpret_cast<const XrSpatialFilterTrackingStateEXT *>(n)
              ->trackingState;
    if (n->type == XR_TYPE_SPATIAL_FILTER_ARCHETYPE_EXT) {
      const auto a =
          reinterpret_cast<const XrSpatialFilterArchetypeEXT *>(n)->archetype;
      archetype_matches =
          a ==
          uint64_t(s.components.count(XR_SPATIAL_COMPONENT_TYPE_MESH_3D_EXT)) *
                  2 +
              s.components.count(XR_SPATIAL_COMPONENT_TYPE_BOUNDED_3D_EXT);
    }
  }
  std::vector<std::pair<XrSpatialEntityIdEXT, XrSpatialEntityTrackingStateEXT>>
      selected;
  auto select = [&](XrSpatialEntityIdEXT entity,
                    XrSpatialEntityTrackingStateEXT state) {
    if (archetype_matches &&
        (state_filter == XR_SPATIAL_ENTITY_TRACKING_STATE_MAX_ENUM_EXT ||
         state_filter == state))
      selected.push_back({entity, state});
  };
  if (s.entity)
    select(s.entity, s.state);
  for (auto entity : s.paused_entities)
    select(entity, XR_SPATIAL_ENTITY_TRACKING_STATE_PAUSED_EXT);
  for (auto entity : s.stopped_entities)
    select(entity, XR_SPATIAL_ENTITY_TRACKING_STATE_STOPPED_EXT);
  const uint32_t count = uint32_t(selected.size());
  out->entityIdCountOutput = out->entityStateCountOutput = count;
  if ((out->entityIdCapacityInput && out->entityIdCapacityInput < count) ||
      (out->entityStateCapacityInput && out->entityStateCapacityInput < count))
    return XR_ERROR_SIZE_INSUFFICIENT;
  if ((out->entityIdCapacityInput && !out->entityIds) ||
      (out->entityStateCapacityInput && !out->entityStates))
    return XR_ERROR_VALIDATION_FAILURE;
  // Validate all chained capacities before writing any component data.
  for (auto *n = static_cast<XrBaseOutStructure *>(out->next); n; n = n->next) {
    if (n->type == XR_TYPE_SPATIAL_COMPONENT_MESH_3D_LIST_EXT) {
      auto *m = reinterpret_cast<XrSpatialComponentMesh3DListEXT *>(n);
      if (!requested.count(XR_SPATIAL_COMPONENT_TYPE_MESH_3D_EXT))
        return XR_ERROR_VALIDATION_FAILURE;
      if (m->meshCount < count)
        return XR_ERROR_SIZE_INSUFFICIENT;
      if (count && !m->meshes)
        return XR_ERROR_VALIDATION_FAILURE;
    } else if (n->type == XR_TYPE_SPATIAL_COMPONENT_BOUNDED_3D_LIST_EXT) {
      auto *b = reinterpret_cast<XrSpatialComponentBounded3DListEXT *>(n);
      if (!requested.count(XR_SPATIAL_COMPONENT_TYPE_BOUNDED_3D_EXT))
        return XR_ERROR_VALIDATION_FAILURE;
      if (b->boundCount < count)
        return XR_ERROR_SIZE_INSUFFICIENT;
      if (count && !b->bounds)
        return XR_ERROR_VALIDATION_FAILURE;
    }
  }
  for (uint32_t i = 0; i < count; ++i) {
    if (out->entityIdCapacityInput)
      out->entityIds[i] = selected[i].first;
    if (out->entityStateCapacityInput)
      out->entityStates[i] = selected[i].second;
    const bool tracked =
        selected[i].second == XR_SPATIAL_ENTITY_TRACKING_STATE_TRACKING_EXT;
    for (auto *n = static_cast<XrBaseOutStructure *>(out->next); n;
         n = n->next) {
      if (n->type == XR_TYPE_SPATIAL_COMPONENT_MESH_3D_LIST_EXT)
        reinterpret_cast<XrSpatialComponentMesh3DListEXT *>(n)->meshes[i] = {
            tracked ? s.origin : XrPosef{{0, 0, 0, 1}, {0, 0, 0}},
            {tracked ? 1u : 3u, XR_SPATIAL_BUFFER_TYPE_VECTOR3F_EXT},
            {tracked ? 2u : 4u, XR_SPATIAL_BUFFER_TYPE_UINT32_EXT}};
      if (n->type == XR_TYPE_SPATIAL_COMPONENT_BOUNDED_3D_LIST_EXT)
        reinterpret_cast<XrSpatialComponentBounded3DListEXT *>(n)->bounds[i] =
            tracked ? s.bounds : XrBoxf{};
    }
  }
  return XR_SUCCESS;
}
API_END
XrResult XRAPI_CALL destroy_snapshot(XrSpatialSnapshotEXT h) try {
  LOCK;
  return snapshots.erase(h) ? XR_SUCCESS : XR_ERROR_HANDLE_INVALID;
}
API_END
XrResult XRAPI_CALL create_entity(
    XrSpatialContextEXT h, const XrSpatialEntityFromIdCreateInfoEXT *info,
    XrSpatialEntityEXT *out) try {
  TYPE(info, XR_TYPE_SPATIAL_ENTITY_FROM_ID_CREATE_INFO_EXT);
  if (!out)
    return XR_ERROR_VALIDATION_FAILURE;
  LOCK;
  if (!contexts.count(h))
    return XR_ERROR_HANDLE_INVALID;
  if (!contexts.at(h).known_ids.count(info->entityId) || !info->entityId)
    return XR_ERROR_SPATIAL_ENTITY_ID_INVALID_EXT;
  if (entities.size() >= handle_limit)
    return XR_ERROR_LIMIT_REACHED;
  *out = handle<XrSpatialEntityEXT>(id());
  entities.emplace(*out, Entity{h, info->entityId});
  return XR_SUCCESS;
}
API_END
XrResult XRAPI_CALL destroy_entity(XrSpatialEntityEXT h) try {
  LOCK;
  return entities.erase(h) ? XR_SUCCESS : XR_ERROR_HANDLE_INVALID;
}
API_END
XrResult XRAPI_CALL create_update(
    XrSpatialContextEXT h, const XrSpatialUpdateSnapshotCreateInfoEXT *info,
    XrSpatialSnapshotEXT *out) try {
  TYPE(info, XR_TYPE_SPATIAL_UPDATE_SNAPSHOT_CREATE_INFO_EXT);
  if (!out || !info->entities || !info->entityCount || info->entityCount > 1024)
    return XR_ERROR_VALIDATION_FAILURE;
  LOCK;
  auto c = contexts.find(h);
  if (c == contexts.end())
    return XR_ERROR_HANDLE_INVALID;
  Future f{};
  f.context = h;
  f.session = c->second.session;
  CHECK(components(info->componentTypeCount, info->componentTypes,
                   c->second.components, f.components));
  std::set<XrSpatialEntityIdEXT> wanted;
  for (uint32_t i = 0; i < info->entityCount; ++i) {
    auto e = entities.find(info->entities[i]);
    if (e == entities.end() || e->second.context != h)
      return XR_ERROR_HANDLE_INVALID;
    wanted.insert(e->second.id);
  }
  if (snapshots.size() >= handle_limit)
    return XR_ERROR_LIMIT_REACHED;
  XrPosef transform;
  CHECK(xr_gate_locate_runtime(f.session, info->baseSpace, info->time,
                               &transform));
  f.sensor = sessions.at(f.session).source->latest();
  auto s = geometry(h, f, f.sensor, transform);
  if (!wanted.erase(s.entity)) {
    s.entity = 0;
    s.state = XR_SPATIAL_ENTITY_TRACKING_STATE_STOPPED_EXT;
    s.vertices.clear();
    s.indices.clear();
  }
  const XrSpatialEntityIdEXT current_id =
      0x5852000000000000ULL + f.sensor.tracking_epoch;
  for (auto entity : wanted) {
    if (entity == current_id)
      s.paused_entities.push_back(entity);
    else
      s.stopped_entities.push_back(entity);
  }
  *out = handle<XrSpatialSnapshotEXT>(id());
  snapshots.emplace(*out, std::move(s));
  return XR_SUCCESS;
}
API_END
XrResult XRAPI_CALL get_vertices(XrSpatialSnapshotEXT h,
                                 const XrSpatialBufferGetInfoEXT *info,
                                 uint32_t cap, uint32_t *count,
                                 XrVector3f *out) try {
  TYPE(info, XR_TYPE_SPATIAL_BUFFER_GET_INFO_EXT);
  LOCK;
  auto s = snapshots.find(h);
  if (s == snapshots.end())
    return XR_ERROR_HANDLE_INVALID;
  if (info->bufferId == 3 && (!s->second.stopped_entities.empty() ||
                              !s->second.paused_entities.empty()))
    return copy(cap, count, out, std::vector<XrVector3f>{});
  if (info->bufferId != 1 || !s->second.entity)
    return XR_ERROR_SPATIAL_BUFFER_ID_INVALID_EXT;
  return copy(cap, count, out, s->second.vertices);
}
API_END
XrResult XRAPI_CALL get_indices(XrSpatialSnapshotEXT h,
                                const XrSpatialBufferGetInfoEXT *info,
                                uint32_t cap, uint32_t *count,
                                uint32_t *out) try {
  TYPE(info, XR_TYPE_SPATIAL_BUFFER_GET_INFO_EXT);
  LOCK;
  auto s = snapshots.find(h);
  if (s == snapshots.end())
    return XR_ERROR_HANDLE_INVALID;
  if (info->bufferId == 4 && (!s->second.stopped_entities.empty() ||
                              !s->second.paused_entities.empty()))
    return copy(cap, count, out, std::vector<uint32_t>{});
  if (info->bufferId != 2 || !s->second.entity)
    return XR_ERROR_SPATIAL_BUFFER_ID_INVALID_EXT;
  return copy(cap, count, out, s->second.indices);
}
API_END
template <class T>
XrResult XRAPI_CALL get_unsupported(XrSpatialSnapshotEXT h,
                                    const XrSpatialBufferGetInfoEXT *info,
                                    uint32_t, uint32_t *, T *) {
  TYPE(info, XR_TYPE_SPATIAL_BUFFER_GET_INFO_EXT);
  LOCK;
  if (!snapshots.count(h))
    return XR_ERROR_HANDLE_INVALID;
  return info->bufferId == 1 || info->bufferId == 2
             ? XR_ERROR_VALIDATION_FAILURE
             : XR_ERROR_SPATIAL_BUFFER_ID_INVALID_EXT;
}
XrResult XRAPI_CALL enumerate_archetypes(XrSpatialSnapshotEXT h, uint32_t cap,
                                         uint32_t *count,
                                         XrSpatialArchetypeIdEXT *out) try {
  LOCK;
  auto s = snapshots.find(h);
  if (s == snapshots.end())
    return XR_ERROR_HANDLE_INVALID;
  const auto &c = s->second.components;
  uint64_t a = uint64_t(c.count(XR_SPATIAL_COMPONENT_TYPE_MESH_3D_EXT)) * 2 +
               c.count(XR_SPATIAL_COMPONENT_TYPE_BOUNDED_3D_EXT);
  return copy(cap, count, out,
              (s->second.entity || (!s->second.stopped_entities.empty() ||
                                    !s->second.paused_entities.empty()))
                  ? std::vector<XrSpatialArchetypeIdEXT>{a}
                  : std::vector<XrSpatialArchetypeIdEXT>{});
}
API_END
XrResult XRAPI_CALL enumerate_archetype_components(
    XrSpatialSnapshotEXT h, XrSpatialArchetypeIdEXT a, uint32_t cap,
    uint32_t *count, XrSpatialComponentTypeEXT *out) try {
  LOCK;
  auto s = snapshots.find(h);
  if (s == snapshots.end())
    return XR_ERROR_HANDLE_INVALID;
  const auto &c = s->second.components;
  if ((!s->second.entity && s->second.stopped_entities.empty() &&
       s->second.paused_entities.empty()) ||
      a != uint64_t(c.count(XR_SPATIAL_COMPONENT_TYPE_MESH_3D_EXT)) * 2 +
               c.count(XR_SPATIAL_COMPONENT_TYPE_BOUNDED_3D_EXT))
    return XR_ERROR_VALIDATION_FAILURE;
  return copy(cap, count, out,
              std::vector<XrSpatialComponentTypeEXT>(c.begin(), c.end()));
}
API_END
XrResult XRAPI_CALL create_raw(XrSession sess,
                               const XrDataSnapshotCreateInfoMNDX *info,
                               XrDataSnapshotMNDX *out) try {
  XrInstance inst;
  CHECK(xr_gate_validate_session(sess, raw_ext, &inst));
  TYPE(info, XR_TYPE_DATA_SNAPSHOT_CREATE_INFO_MNDX);
  if (!out)
    return XR_ERROR_VALIDATION_FAILURE;
  LOCK;
  if (raw_snapshots.size() >= 64)
    return XR_ERROR_LIMIT_REACHED;
  XrPosef transform;
  CHECK(xr_gate_locate_runtime(sess, info->baseSpace, info->time, &transform));
  *out = handle<XrDataSnapshotMNDX>(id());
  raw_snapshots.emplace(
      *out, Raw{sess, session(sess, inst).source->latest(), transform});
  return XR_SUCCESS;
}
API_END
XrResult XRAPI_CALL raw_properties(XrDataSnapshotMNDX h,
                                   XrDataSnapshotPropertiesMNDX *out) try {
  TYPE(out, XR_TYPE_DATA_SNAPSHOT_PROPERTIES_MNDX);
  LOCK;
  auto it = raw_snapshots.find(h);
  if (it == raw_snapshots.end())
    return XR_ERROR_HANDLE_INVALID;
  const auto &s = it->second.sensor;
  out->spatialAvailable = bool(s.mesh);
  out->videoAvailable = bool(s.video);
  out->spatialTrackingValid = s.mesh_world_valid;
  out->trackingEpoch = s.tracking_epoch;
  out->runtimeToBase = it->second.transform;
  out->spatialSourceTimestampNs = s.mesh ? s.mesh->source_timestamp_ns : 0;
  out->videoSourceTimestampNs =
      s.video ? s.video->header.source_timestamp_ns : 0;
  return XR_SUCCESS;
}
API_END
XrResult XRAPI_CALL raw_mesh(XrDataSnapshotMNDX h, uint32_t cap,
                             uint32_t *count, uint8_t *out) try {
  LOCK;
  auto it = raw_snapshots.find(h);
  if (it == raw_snapshots.end())
    return XR_ERROR_HANDLE_INVALID;
  const auto &m = it->second.sensor.mesh;
  const uint32_t size = m ? sizeof(*m) : 0;
  if (!count)
    return XR_ERROR_VALIDATION_FAILURE;
  *count = size;
  if (!cap)
    return XR_SUCCESS;
  if (cap < size)
    return XR_ERROR_SIZE_INSUFFICIENT;
  if (size && !out)
    return XR_ERROR_VALIDATION_FAILURE;
  if (size)
    std::memcpy(out, m.get(), size);
  return XR_SUCCESS;
}
API_END
XrResult XRAPI_CALL raw_video(XrDataSnapshotMNDX h, uint32_t cap,
                              uint32_t *count, uint8_t *out) try {
  LOCK;
  auto it = raw_snapshots.find(h);
  if (it == raw_snapshots.end())
    return XR_ERROR_HANDLE_INVALID;
  const auto &v = it->second.sensor.video;
  const uint32_t size =
      v ? uint32_t(sizeof(v->header) + v->left.size() + v->right.size()) : 0;
  if (!count)
    return XR_ERROR_VALIDATION_FAILURE;
  *count = size;
  if (!cap)
    return XR_SUCCESS;
  if (cap < size)
    return XR_ERROR_SIZE_INSUFFICIENT;
  if (size && !out)
    return XR_ERROR_VALIDATION_FAILURE;
  if (size)
    xr_video::write_frame_payload(out, *v);
  return XR_SUCCESS;
}
API_END
XrResult XRAPI_CALL destroy_raw(XrDataSnapshotMNDX h) try {
  LOCK;
  return raw_snapshots.erase(h) ? XR_SUCCESS : XR_ERROR_HANDLE_INVALID;
}
API_END
} // namespace
extern "C" bool xr_gate_extension_available(const char *name) {
  if (!name)
    return false;
  if (!std::strcmp(name, entity_ext) || !std::strcmp(name, mesh_ext) ||
      !std::strcmp(name, XR_EXT_SPATIAL_ENTITY_ARCHETYPES_EXTENSION_NAME))
    return xr::monado_driver::option("XR_MONADO_SPATIAL");
  if (!std::strcmp(name, raw_ext))
    return xr::monado_driver::option("XR_MONADO_SPATIAL") ||
           xr::monado_driver::option("XR_MONADO_VIDEO");
  return true;
}
extern "C" XrResult xr_gate_get_proc_addr(XrInstance instance, const char *name,
                                          PFN_xrVoidFunction *function) try {
  if (!name || !function)
    return XR_ERROR_VALIDATION_FAILURE;
#define ENTRY(api, impl, ext)                                                  \
  if (!std::strcmp(name, #api)) {                                              \
    CHECK(xr_gate_validate_instance(instance, ext, XR_NULL_SYSTEM_ID));        \
    *function = reinterpret_cast<PFN_xrVoidFunction>(impl);                    \
    return XR_SUCCESS;                                                         \
  }
  ENTRY(xrEnumerateSpatialCapabilitiesEXT, enumerate_capabilities, entity_ext)
  ENTRY(xrEnumerateSpatialCapabilityComponentTypesEXT, enumerate_components,
        entity_ext)
  ENTRY(xrEnumerateSpatialCapabilityFeaturesEXT, enumerate_features, entity_ext)
  ENTRY(xrCreateSpatialContextAsyncEXT, create_context, entity_ext)
  ENTRY(xrCreateSpatialContextCompleteEXT, complete_context, entity_ext)
  ENTRY(xrDestroySpatialContextEXT, destroy_context, entity_ext)
  ENTRY(xrCreateSpatialDiscoverySnapshotAsyncEXT, create_discovery, entity_ext)
  ENTRY(xrCreateSpatialDiscoverySnapshotCompleteEXT, complete_discovery,
        entity_ext)
  ENTRY(xrQuerySpatialComponentDataEXT, query, entity_ext)
  ENTRY(xrDestroySpatialSnapshotEXT, destroy_snapshot, entity_ext)
  ENTRY(xrCreateSpatialEntityFromIdEXT, create_entity, entity_ext)
  ENTRY(xrDestroySpatialEntityEXT, destroy_entity, entity_ext)
  ENTRY(xrCreateSpatialUpdateSnapshotEXT, create_update, entity_ext)
  ENTRY(xrGetSpatialBufferVector3fEXT, get_vertices, entity_ext)
  ENTRY(xrGetSpatialBufferUint32EXT, get_indices, entity_ext)
  ENTRY(xrGetSpatialBufferStringEXT, get_unsupported<char>, entity_ext)
  ENTRY(xrGetSpatialBufferUint8EXT, get_unsupported<uint8_t>, entity_ext)
  ENTRY(xrGetSpatialBufferUint16EXT, get_unsupported<uint16_t>, entity_ext)
  ENTRY(xrGetSpatialBufferFloatEXT, get_unsupported<float>, entity_ext)
  ENTRY(xrGetSpatialBufferVector2fEXT, get_unsupported<XrVector2f>, entity_ext)
  ENTRY(xrEnumerateSpatialArchetypesEXT, enumerate_archetypes,
        XR_EXT_SPATIAL_ENTITY_ARCHETYPES_EXTENSION_NAME)
  ENTRY(xrEnumerateSpatialArchetypeComponentTypesEXT,
        enumerate_archetype_components,
        XR_EXT_SPATIAL_ENTITY_ARCHETYPES_EXTENSION_NAME)
  ENTRY(xrCreateDataSnapshotMNDX, create_raw, raw_ext)
  ENTRY(xrGetDataSnapshotPropertiesMNDX, raw_properties, raw_ext)
  ENTRY(xrGetSpatialMeshMNDX, raw_mesh, raw_ext)
  ENTRY(xrGetStereoVideoMNDX, raw_video, raw_ext)
  ENTRY(xrDestroyDataSnapshotMNDX, destroy_raw, raw_ext)
#undef ENTRY
  return XR_ERROR_FUNCTION_UNSUPPORTED;
}
API_END
extern "C" bool xr_gate_reacquired(XrSession sess) {
  if (!xr::monado_driver::option("XR_MONADO_RECENTER_ON_REACQUIRE", true))
    return false;
  try {
    XrInstance inst;
    if (XR_FAILED(xr_gate_validate_session(sess, nullptr, &inst)))
      return false;
    LOCK;
    auto &s = session(sess, inst);
    const auto sample = s.source->latest();
    if (!sample.tracking_valid || sample.tracking_epoch == s.recentered_epoch)
      return false;
    s.recentered_epoch = sample.tracking_epoch;
    return true;
  } catch (...) {
    return false;
  }
}
extern "C" void xr_gate_destroy_session(XrSession sess) {
  LOCK;
  for (auto it = contexts.begin(); it != contexts.end();)
    if (it->second.session == sess) {
      const auto h = it->first;
      ++it;
      destroy_context(h);
    } else
      ++it;
  erase_session(futures, sess);
  erase_session(raw_snapshots, sess);
  sessions.erase(sess);
}
extern "C" void xr_gate_destroy_instance(XrInstance inst) {
  LOCK;
  for (auto it = sessions.begin(); it != sessions.end();)
    if (it->second.instance == inst) {
      const auto h = it->first;
      ++it;
      xr_gate_destroy_session(h);
    } else
      ++it;
}
extern "C" bool xr_gate_owns_future(XrFutureEXT f) {
  return (uint64_t(reinterpret_cast<uintptr_t>(f)) & 0xffffff0000000000ULL) ==
         future_tag;
}
extern "C" XrResult xr_gate_poll_future(XrInstance inst,
                                        const XrFuturePollInfoEXT *info,
                                        XrFuturePollResultEXT *out) try {
  TYPE(info, XR_TYPE_FUTURE_POLL_INFO_EXT);
  TYPE(out, XR_TYPE_FUTURE_POLL_RESULT_EXT);
  LOCK;
  auto f = futures.find(info->future);
  if (f == futures.end() || sessions.at(f->second.session).instance != inst)
    return XR_ERROR_FUTURE_INVALID_EXT;
  out->state = XR_FUTURE_STATE_READY_EXT;
  return XR_SUCCESS;
}
API_END
extern "C" XrResult
xr_gate_cancel_future(XrInstance inst, const XrFutureCancelInfoEXT *info) try {
  TYPE(info, XR_TYPE_FUTURE_CANCEL_INFO_EXT);
  LOCK;
  auto f = futures.find(info->future);
  if (f == futures.end() || sessions.at(f->second.session).instance != inst)
    return XR_ERROR_FUTURE_INVALID_EXT;
  futures.erase(f);
  return XR_SUCCESS;
}
API_END
extern "C" bool xr_gate_poll_event(XrInstance inst, XrEventDataBuffer *event) {
  try {
    LOCK;
    for (auto &[h, c] : contexts) {
      auto &s = sessions.at(c.session);
      if (s.instance != inst || c.event_sent)
        continue;
      auto data = s.source->latest();
      const auto seq = data.mesh ? data.mesh->sequence : 0;
      if (seq == c.discovery_sequence &&
          data.tracking_epoch == c.discovery_epoch)
        continue;
      XrEventDataSpatialDiscoveryRecommendedEXT ev{
          XR_TYPE_EVENT_DATA_SPATIAL_DISCOVERY_RECOMMENDED_EXT, nullptr, h};
      std::memcpy(event, &ev, sizeof(ev));
      c.event_sent = true;
      return true;
    }
  } catch (...) {
  }
  return false;
}
