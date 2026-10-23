#pragma once
#include "spatial_compat.h"
#include "xr_gate_data.h"
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
// Implemented in the Monado C adapter, using Monado handle/session validation.
XrResult xr_gate_validate_instance(XrInstance instance, const char *extension,
                                   XrSystemId system);
XrResult xr_gate_validate_session(XrSession session, const char *extension,
                                  XrInstance *instance);
XrResult xr_gate_locate_runtime(XrSession session, XrSpace base, XrTime time,
                                XrPosef *pose);
// Implemented in the project-owned C++ extension module.
bool xr_gate_extension_available(const char *name);
XrResult xr_gate_get_proc_addr(XrInstance instance, const char *name,
                               PFN_xrVoidFunction *function);
bool xr_gate_reacquired(XrSession session);
void xr_gate_destroy_session(XrSession session);
void xr_gate_destroy_instance(XrInstance instance);
bool xr_gate_poll_event(XrInstance instance, XrEventDataBuffer *event);
bool xr_gate_owns_future(XrFutureEXT future);
XrResult xr_gate_poll_future(XrInstance instance,
                             const XrFuturePollInfoEXT *info,
                             XrFuturePollResultEXT *result);
XrResult xr_gate_cancel_future(XrInstance instance,
                               const XrFutureCancelInfoEXT *info);
#ifdef __cplusplus
}
#endif
