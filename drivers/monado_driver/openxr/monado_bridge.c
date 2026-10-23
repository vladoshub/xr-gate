// Monado-internal validation/space adapter. Kept in C so project C++ code never
// depends on Monado's C-only atomics or layout of oxr_* objects.
#include "oxr_api_verify.h"
#include "oxr_bridge.h"
#include "oxr_logger.h"
#include "oxr_objects.h"
#include "oxr_roles.h"
#include <string.h>

XrResult xr_gate_validate_instance(XrInstance instance, const char *extension,
                                   XrSystemId system) {
  struct oxr_instance *inst;
  struct oxr_logger log;
  OXR_VERIFY_INSTANCE_AND_INIT_LOG(&log, instance, inst, "xr_gate_data");
  if (system != XR_NULL_SYSTEM_ID) {
    XrResult ret = oxr_system_verify_id(&log, inst, system);
    if (XR_FAILED(ret))
      return ret;
  }
  if (extension == NULL)
    return XR_SUCCESS;
  bool enabled = false;
  if (!strcmp(extension, XR_EXT_SPATIAL_ENTITY_EXTENSION_NAME))
    enabled = inst->extensions.EXT_spatial_entity;
  if (!strcmp(extension, XR_EXT_SPATIAL_MESH_TRACKING_EXTENSION_NAME))
    enabled = inst->extensions.EXT_spatial_mesh_tracking;
  if (!strcmp(extension, XR_EXT_SPATIAL_ENTITY_ARCHETYPES_EXTENSION_NAME))
    enabled = inst->extensions.EXT_spatial_entity_archetypes;
  if (!strcmp(extension, XR_MNDX_XR_GATE_DATA_EXTENSION_NAME))
    enabled = inst->extensions.MNDX_xr_gate_data;
  return enabled ? XR_SUCCESS : XR_ERROR_FUNCTION_UNSUPPORTED;
}
XrResult xr_gate_validate_session(XrSession session, const char *extension,
                                  XrInstance *instance) {
  struct oxr_session *sess;
  struct oxr_logger log;
  OXR_VERIFY_SESSION_AND_INIT_LOG(&log, session, sess, "xr_gate_data");
  OXR_VERIFY_SESSION_NOT_LOST(&log, sess);
  // Only this driver publishes runtime_local data. Do not expose the stream in
  // unrelated Monado systems that happen to run in the same user environment.
  struct xrt_device *head = GET_STATIC_XDEV_BY_ROLE(sess->sys, head);
  if (!head || strcmp(head->serial, "xr-tracking-monado-hmd-001"))
    return XR_ERROR_FEATURE_UNSUPPORTED;
  *instance = oxr_instance_to_openxr(sess->sys->inst);
  return xr_gate_validate_instance(*instance, extension, XR_NULL_SYSTEM_ID);
}
XrResult xr_gate_locate_runtime(XrSession session, XrSpace base, XrTime time,
                                XrPosef *pose) {
  struct oxr_session *sess;
  struct oxr_space *space;
  struct oxr_logger log;
  OXR_VERIFY_SESSION_AND_INIT_LOG(&log, session, sess,
                                  "xr_gate_locate_runtime");
  OXR_VERIFY_SPACE_AND_INIT_LOG(&log, base, space, "xr_gate_locate_runtime");
  if (space->sess != sess)
    return XR_ERROR_HANDLE_INVALID;
  if (time <= 0)
    return XR_ERROR_TIME_INVALID;
  struct xrt_space_relation rel = XRT_SPACE_RELATION_ZERO;
  XrResult ret = oxr_space_locate_device(
      &log, GET_STATIC_XDEV_BY_ROLE(sess->sys, head), space, time, &rel);
  if (XR_FAILED(ret))
    return ret;
  const enum xrt_space_relation_flags required =
      XRT_SPACE_RELATION_ORIENTATION_VALID_BIT |
      XRT_SPACE_RELATION_POSITION_VALID_BIT;
  if ((rel.relation_flags & required) != required)
    return XR_ERROR_POSE_INVALID;
  memcpy(&pose->orientation, &rel.pose.orientation, sizeof(pose->orientation));
  memcpy(&pose->position, &rel.pose.position, sizeof(pose->position));
  return XR_SUCCESS;
}
