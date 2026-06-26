#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

struct xrt_device;

struct xrt_device* xr_tracking_runtime_hmd_create(void);
struct xrt_device* xr_tracking_runtime_controller_create(struct xrt_device* hmd_xdev, bool left);

#ifdef __cplusplus
}
#endif
