#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct xrt_device;
struct xrt_tracking_origin;

struct xrt_device* xr_tracking_runtime_hmd_create(void);
struct xrt_device* xr_tracking_runtime_controller_create(struct xrt_device* hmd_xdev, bool left);
uint32_t xr_tracking_runtime_body_trackers_create(struct xrt_tracking_origin* origin,
                                                 struct xrt_device** out_devices,
                                                 uint32_t capacity);

#ifdef __cplusplus
}
#endif
