// Copyright 2026
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <stdint.h>

#ifdef _WIN32
#ifdef XR_MERCURY_RUNTIME_BUILD
#define XR_MERCURY_RUNTIME_EXPORT __declspec(dllexport)
#else
#define XR_MERCURY_RUNTIME_EXPORT __declspec(dllimport)
#endif
#else
#define XR_MERCURY_RUNTIME_EXPORT __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define XR_MERCURY_RUNTIME_ABI_VERSION 1u
#define XR_MERCURY_HAND_JOINT_COUNT 21u

#pragma pack(push, 1)

struct xr_mercury_joint_f32
{
	uint32_t joint_id;
	uint32_t flags;
	float px;
	float py;
	float pz;
	float qw;
	float qx;
	float qy;
	float qz;
	float radius_m;
	float confidence;
};

struct xr_mercury_hand_side_f32
{
	uint32_t handedness;
	uint32_t status;
	uint32_t flags;
	float confidence;

	float controller_px;
	float controller_py;
	float controller_pz;
	float controller_qw;
	float controller_qx;
	float controller_qy;
	float controller_qz;

	float palm_px;
	float palm_py;
	float palm_pz;
	float palm_qw;
	float palm_qx;
	float palm_qy;
	float palm_qz;

	float wrist_px;
	float wrist_py;
	float wrist_pz;
	float wrist_qw;
	float wrist_qx;
	float wrist_qy;
	float wrist_qz;

	float vx;
	float vy;
	float vz;
	float wx;
	float wy;
	float wz;

	float pinch_strength;
	float grab_strength;
	uint32_t pinch_active;
	uint32_t grab_active;

	uint32_t joint_count;
	uint32_t reserved0;

	struct xr_mercury_joint_f32 joints[XR_MERCURY_HAND_JOINT_COUNT];
};

struct xr_mercury_frame_f32
{
	uint32_t version;
	uint32_t size_bytes;

	uint64_t sequence;
	uint64_t timestamp_ns;
	uint64_t source_timestamp_ns;
	uint64_t reset_counter;

	uint32_t tracking_status;
	uint32_t flags;
	float confidence;
	uint32_t hand_count;

	struct xr_mercury_hand_side_f32 left;
	struct xr_mercury_hand_side_f32 right;
};

struct xr_mercury_create_info
{
	uint32_t abi_version;
	uint32_t size_bytes;
	const char *models_dir;
	const char *calib_json;
	uint32_t flags;
	int32_t orientation0;
	int32_t orientation1;
	int32_t swap_cameras;
	int32_t boundary_circle;
	float boundary0_center_x;
	float boundary0_center_y;
	float boundary0_radius;
	float boundary1_center_x;
	float boundary1_center_y;
	float boundary1_radius;
};

#pragma pack(pop)

XR_MERCURY_RUNTIME_EXPORT void *
xr_mercury_runtime_create(const struct xr_mercury_create_info *info);

XR_MERCURY_RUNTIME_EXPORT void
xr_mercury_runtime_destroy(void *ctx);

XR_MERCURY_RUNTIME_EXPORT int
xr_mercury_runtime_process_gray8(void *ctx,
                                    const uint8_t *cam0,
                                    uint32_t cam0_width,
                                    uint32_t cam0_height,
                                    uint32_t cam0_stride,
                                    const uint8_t *cam1,
                                    uint32_t cam1_width,
                                    uint32_t cam1_height,
                                    uint32_t cam1_stride,
                                    uint64_t source_timestamp_ns,
                                    uint64_t sequence,
                                    struct xr_mercury_frame_f32 *out_frame);

XR_MERCURY_RUNTIME_EXPORT const char *
xr_mercury_runtime_last_error(void *ctx);

#ifdef __cplusplus
} // extern "C"
#endif
