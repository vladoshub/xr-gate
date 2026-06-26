// Copyright 2026
// SPDX-License-Identifier: BSL-1.0
#include "xr_mercury_runtime_c_api.h"

#include "hg_interface.h"

#include "tracking/t_hand_tracking.h"
#include "tracking/t_tracking.h"
#include "util/u_frame.h"
#include "xrt/xrt_defines.h"
#include "xrt/xrt_frame.h"

#include <cjson/cJSON.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr uint32_t HAND_STATUS_NO_HANDS = 0;
constexpr uint32_t HAND_STATUS_TRACKING = 1;
constexpr uint32_t HAND_FLAG_LEFT_VALID = 1u << 0;
constexpr uint32_t HAND_FLAG_RIGHT_VALID = 1u << 1;
constexpr uint32_t HAND_FLAG_JOINTS_VALID = 1u << 2;
constexpr uint32_t HAND_POSE_VALID = 1u << 0;
constexpr uint32_t HAND_JOINTS_VALID = 1u << 3;

struct JsonDoc
{
	cJSON *root = nullptr;
	explicit JsonDoc(const fs::path &path)
	{
		std::ifstream f(path);
		if (!f) {
			throw std::runtime_error("failed to open calibration JSON: " + path.string());
		}
		std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
		root = cJSON_Parse(s.c_str());
		if (!root) {
			throw std::runtime_error("failed to parse calibration JSON: " + path.string());
		}
	}
	~JsonDoc() { cJSON_Delete(root); }
};

struct QuatPose
{
	double px = 0, py = 0, pz = 0;
	double qx = 0, qy = 0, qz = 0, qw = 1;
};

struct Mat3
{
	double v[3][3]{};
};

struct Vec3
{
	double v[3]{};
};

static cJSON *
obj(cJSON *parent, const char *name)
{
	cJSON *v = cJSON_GetObjectItemCaseSensitive(parent, name);
	if (!v) {
		throw std::runtime_error(std::string("missing JSON field: ") + name);
	}
	return v;
}

static cJSON *
arr_item(cJSON *array, int idx, const char *name)
{
	if (!cJSON_IsArray(array)) {
		throw std::runtime_error(std::string("expected JSON array: ") + name);
	}
	cJSON *v = cJSON_GetArrayItem(array, idx);
	if (!v) {
		throw std::runtime_error(std::string("missing JSON array item: ") + name);
	}
	return v;
}

static double
num_obj(cJSON *parent, const char *name)
{
	cJSON *v = obj(parent, name);
	if (!cJSON_IsNumber(v)) {
		throw std::runtime_error(std::string("expected numeric JSON field: ") + name);
	}
	return v->valuedouble;
}

static double
num_arr(cJSON *array, int idx, const char *name)
{
	cJSON *v = arr_item(array, idx, name);
	if (!cJSON_IsNumber(v)) {
		throw std::runtime_error(std::string("expected numeric JSON array item: ") + name);
	}
	return v->valuedouble;
}

static QuatPose
read_pose(cJSON *pose)
{
	QuatPose p;
	p.px = num_obj(pose, "px");
	p.py = num_obj(pose, "py");
	p.pz = num_obj(pose, "pz");
	p.qx = num_obj(pose, "qx");
	p.qy = num_obj(pose, "qy");
	p.qz = num_obj(pose, "qz");
	p.qw = num_obj(pose, "qw");
	return p;
}

static Mat3
quat_to_mat(const QuatPose &q0)
{
	double x = q0.qx, y = q0.qy, z = q0.qz, w = q0.qw;
	double n = std::sqrt(x * x + y * y + z * z + w * w);
	if (n <= 0.0) {
		throw std::runtime_error("zero quaternion in calibration");
	}
	x /= n; y /= n; z /= n; w /= n;

	Mat3 r;
	r.v[0][0] = 1.0 - 2.0 * (y * y + z * z);
	r.v[0][1] = 2.0 * (x * y - z * w);
	r.v[0][2] = 2.0 * (x * z + y * w);
	r.v[1][0] = 2.0 * (x * y + z * w);
	r.v[1][1] = 1.0 - 2.0 * (x * x + z * z);
	r.v[1][2] = 2.0 * (y * z - x * w);
	r.v[2][0] = 2.0 * (x * z - y * w);
	r.v[2][1] = 2.0 * (y * z + x * w);
	r.v[2][2] = 1.0 - 2.0 * (x * x + y * y);
	return r;
}

static Mat3 transpose(const Mat3 &a)
{
	Mat3 r;
	for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) r.v[i][j] = a.v[j][i];
	return r;
}

static Mat3 mul(const Mat3 &a, const Mat3 &b)
{
	Mat3 r;
	for (int i = 0; i < 3; i++) {
		for (int j = 0; j < 3; j++) {
			r.v[i][j] = 0.0;
			for (int k = 0; k < 3; k++) r.v[i][j] += a.v[i][k] * b.v[k][j];
		}
	}
	return r;
}

static Vec3 mul(const Mat3 &a, const Vec3 &b)
{
	return Vec3{{a.v[0][0] * b.v[0] + a.v[0][1] * b.v[1] + a.v[0][2] * b.v[2],
	             a.v[1][0] * b.v[0] + a.v[1][1] * b.v[1] + a.v[1][2] * b.v[2],
	             a.v[2][0] * b.v[0] + a.v[2][1] * b.v[1] + a.v[2][2] * b.v[2]}};
}

static Vec3 sub(const Vec3 &a, const Vec3 &b)
{
	return Vec3{{a.v[0] - b.v[0], a.v[1] - b.v[1], a.v[2] - b.v[2]}};
}

static Vec3 pose_t(const QuatPose &p) { return Vec3{{p.px, p.py, p.pz}}; }

static void
fill_camera_calibration(struct t_camera_calibration &out, cJSON *intr_node, cJSON *res_node)
{
	cJSON *intr = obj(intr_node, "intrinsics");
	int w = (int)num_arr(res_node, 0, "resolution[0]");
	int h = (int)num_arr(res_node, 1, "resolution[1]");

	out.image_size_pixels.w = w;
	out.image_size_pixels.h = h;

	out.intrinsics[0][0] = num_obj(intr, "fx");
	out.intrinsics[0][1] = 0.0;
	out.intrinsics[0][2] = num_obj(intr, "cx");
	out.intrinsics[1][0] = 0.0;
	out.intrinsics[1][1] = num_obj(intr, "fy");
	out.intrinsics[1][2] = num_obj(intr, "cy");
	out.intrinsics[2][0] = 0.0;
	out.intrinsics[2][1] = 0.0;
	out.intrinsics[2][2] = 1.0;

	out.distortion_model = T_DISTORTION_FISHEYE_KB4;
	out.kb4.k1 = num_obj(intr, "k1");
	out.kb4.k2 = num_obj(intr, "k2");
	out.kb4.k3 = num_obj(intr, "k3");
	out.kb4.k4 = num_obj(intr, "k4");
}

static struct t_stereo_camera_calibration *
load_stereo_calibration_json(const fs::path &path)
{
	JsonDoc doc(path);
	cJSON *value0 = obj(doc.root, "value0");
	cJSON *intrinsics = obj(value0, "intrinsics");
	cJSON *resolution = obj(value0, "resolution");
	cJSON *t_imu_cam = obj(value0, "T_imu_cam");

	struct t_stereo_camera_calibration *calib = nullptr;
	t_stereo_camera_calibration_alloc(&calib, T_DISTORTION_FISHEYE_KB4);
	if (!calib) throw std::runtime_error("t_stereo_camera_calibration_alloc failed");

	fill_camera_calibration(calib->view[0], arr_item(intrinsics, 0, "intrinsics[0]"), arr_item(resolution, 0, "resolution[0]"));
	fill_camera_calibration(calib->view[1], arr_item(intrinsics, 1, "intrinsics[1]"), arr_item(resolution, 1, "resolution[1]"));

	QuatPose p0 = read_pose(arr_item(t_imu_cam, 0, "T_imu_cam[0]"));
	QuatPose p1 = read_pose(arr_item(t_imu_cam, 1, "T_imu_cam[1]"));
	Mat3 R_i_c0 = quat_to_mat(p0);
	Mat3 R_i_c1 = quat_to_mat(p1);
	Mat3 R_c1_i = transpose(R_i_c1);
	Mat3 R_c1_c0 = mul(R_c1_i, R_i_c0);
	Vec3 t_c1_c0 = mul(R_c1_i, sub(pose_t(p0), pose_t(p1)));

	for (int i = 0; i < 3; i++) {
		calib->camera_translation[i] = t_c1_c0.v[i];
		for (int j = 0; j < 3; j++) {
			calib->camera_rotation[i][j] = R_c1_c0.v[i][j];
			calib->camera_essential[i][j] = 0.0;
			calib->camera_fundamental[i][j] = 0.0;
		}
	}
	return calib;
}

static enum t_camera_orientation
orientation_from_int(int32_t v)
{
	switch (v) {
	case 0: return CAMERA_ORIENTATION_0;
	case 90: return CAMERA_ORIENTATION_90;
	case 180: return CAMERA_ORIENTATION_180;
	case 270: return CAMERA_ORIENTATION_270;
	default: throw std::runtime_error("bad Mercury camera orientation, expected 0/90/180/270");
	}
}

static void
apply_boundary_info(struct t_camera_extra_info_one_view &view, float cx, float cy, float radius)
{
	view.boundary_type = HT_IMAGE_BOUNDARY_CIRCLE;
	view.boundary.circle.normalized_center.x = cx;
	view.boundary.circle.normalized_center.y = cy;
	view.boundary.circle.normalized_radius = radius;
}

static struct xrt_frame *
make_frame_from_gray8(const uint8_t *pixels, uint32_t width, uint32_t height, uint32_t stride, uint64_t timestamp_ns, uint64_t sequence)
{
	if (!pixels || width == 0 || height == 0 || stride < width) {
		throw std::runtime_error("bad gray8 input frame");
	}
	struct xrt_frame *frame = nullptr;
	u_frame_create_one_off(XRT_FORMAT_L8, width, height, &frame);
	if (!frame) throw std::runtime_error("u_frame_create_one_off failed");
	for (uint32_t y = 0; y < height; y++) {
		std::memcpy(frame->data + y * frame->stride, pixels + (size_t)y * stride, width);
	}
	frame->timestamp = (int64_t)timestamp_ns;
	frame->source_timestamp = (int64_t)timestamp_ns;
	frame->source_sequence = sequence;
	frame->source_id = 0;
	return frame;
}

struct RuntimeContext
{
	std::string last_error;
	std::string models_dir;
	std::unique_ptr<struct t_stereo_camera_calibration, void (*)(struct t_stereo_camera_calibration *)> calib{nullptr, [](struct t_stereo_camera_calibration *p) {
		struct t_stereo_camera_calibration *tmp = p;
		t_stereo_camera_calibration_reference(&tmp, nullptr);
	}};
	struct t_hand_tracking_sync *sync = nullptr;
	bool swap_cameras = false;

	~RuntimeContext()
	{
		if (sync) t_ht_sync_destroy(&sync);
	}
};

static void
set_identity_joint(struct xr_mercury_joint_f32 &j, uint32_t joint_id)
{
	j = {};
	j.joint_id = joint_id;
	j.qw = 1.0f;
}

static void
init_side(struct xr_mercury_hand_side_f32 &side, uint32_t handedness)
{
	side = {};
	side.handedness = handedness;
	side.status = HAND_STATUS_NO_HANDS;
	side.controller_qw = 1.0f;
	side.palm_qw = 1.0f;
	side.wrist_qw = 1.0f;
	side.joint_count = XR_MERCURY_HAND_JOINT_COUNT;
	for (uint32_t i = 0; i < XR_MERCURY_HAND_JOINT_COUNT; i++) set_identity_joint(side.joints[i], i);
}

static void
init_frame(struct xr_mercury_frame_f32 &frame, uint64_t source_timestamp_ns, uint64_t sequence)
{
	frame = {};
	frame.version = 2;
	frame.size_bytes = sizeof(frame);
	frame.sequence = sequence;
	frame.timestamp_ns = source_timestamp_ns;
	frame.source_timestamp_ns = source_timestamp_ns;
	frame.tracking_status = HAND_STATUS_NO_HANDS;
	init_side(frame.left, 1);
	init_side(frame.right, 2);
}

static void
copy_pose(float &px, float &py, float &pz, float &qw, float &qx, float &qy, float &qz, const xrt_pose &pose)
{
	px = pose.position.x;
	py = pose.position.y;
	pz = pose.position.z;
	qw = pose.orientation.w;
	qx = pose.orientation.x;
	qy = pose.orientation.y;
	qz = pose.orientation.z;
}

static constexpr std::array<enum xrt_hand_joint, XR_MERCURY_HAND_JOINT_COUNT> JOINT_MAP = {{
    XRT_HAND_JOINT_WRIST,
    XRT_HAND_JOINT_THUMB_METACARPAL,
    XRT_HAND_JOINT_THUMB_PROXIMAL,
    XRT_HAND_JOINT_THUMB_DISTAL,
    XRT_HAND_JOINT_THUMB_TIP,
    XRT_HAND_JOINT_INDEX_PROXIMAL,
    XRT_HAND_JOINT_INDEX_INTERMEDIATE,
    XRT_HAND_JOINT_INDEX_DISTAL,
    XRT_HAND_JOINT_INDEX_TIP,
    XRT_HAND_JOINT_MIDDLE_PROXIMAL,
    XRT_HAND_JOINT_MIDDLE_INTERMEDIATE,
    XRT_HAND_JOINT_MIDDLE_DISTAL,
    XRT_HAND_JOINT_MIDDLE_TIP,
    XRT_HAND_JOINT_RING_PROXIMAL,
    XRT_HAND_JOINT_RING_INTERMEDIATE,
    XRT_HAND_JOINT_RING_DISTAL,
    XRT_HAND_JOINT_RING_TIP,
    XRT_HAND_JOINT_LITTLE_PROXIMAL,
    XRT_HAND_JOINT_LITTLE_INTERMEDIATE,
    XRT_HAND_JOINT_LITTLE_DISTAL,
    XRT_HAND_JOINT_LITTLE_TIP,
}};

static void
fill_side(struct xr_mercury_hand_side_f32 &dst, const struct xrt_hand_joint_set &src, uint32_t handedness)
{
	init_side(dst, handedness);
	if (!src.is_active) return;

	dst.status = HAND_STATUS_TRACKING;
	dst.flags = HAND_POSE_VALID | HAND_JOINTS_VALID;
	dst.confidence = 1.0f;

	const auto &palm = src.values.hand_joint_set_default[XRT_HAND_JOINT_PALM].relation;
	const auto &wrist = src.values.hand_joint_set_default[XRT_HAND_JOINT_WRIST].relation;
	copy_pose(dst.palm_px, dst.palm_py, dst.palm_pz, dst.palm_qw, dst.palm_qx, dst.palm_qy, dst.palm_qz, palm.pose);
	copy_pose(dst.wrist_px, dst.wrist_py, dst.wrist_pz, dst.wrist_qw, dst.wrist_qx, dst.wrist_qy, dst.wrist_qz, wrist.pose);
	copy_pose(dst.controller_px, dst.controller_py, dst.controller_pz,
	          dst.controller_qw, dst.controller_qx, dst.controller_qy, dst.controller_qz, palm.pose);

	for (uint32_t i = 0; i < XR_MERCURY_HAND_JOINT_COUNT; i++) {
		const auto joint = JOINT_MAP[i];
		const auto &rel = src.values.hand_joint_set_default[joint].relation;
		auto &out = dst.joints[i];
		out.joint_id = i;
		out.flags = (uint32_t)rel.relation_flags;
		copy_pose(out.px, out.py, out.pz, out.qw, out.qx, out.qy, out.qz, rel.pose);
		out.radius_m = 0.0f;
		out.confidence = (rel.relation_flags != 0) ? 1.0f : 0.0f;
	}
}

} // namespace

extern "C" XR_MERCURY_RUNTIME_EXPORT void *
xr_mercury_runtime_create(const struct xr_mercury_create_info *info)
{
	RuntimeContext *ctx = new RuntimeContext();
	try {
		if (!info || info->abi_version != XR_MERCURY_RUNTIME_ABI_VERSION || info->size_bytes != sizeof(*info)) {
			throw std::runtime_error("bad xr_mercury_create_info ABI");
		}
		if (!info->models_dir || !info->models_dir[0]) throw std::runtime_error("models_dir is empty");
		if (!info->calib_json || !info->calib_json[0]) throw std::runtime_error("calib_json is empty");

		ctx->models_dir = info->models_dir;
		ctx->swap_cameras = info->swap_cameras != 0;
		ctx->calib.reset(load_stereo_calibration_json(info->calib_json));

		struct t_hand_tracking_create_info create_info = {};
		create_info.masks_sink = nullptr;
		if (info->boundary_circle) {
			apply_boundary_info(create_info.cams_info.views[0], info->boundary0_center_x, info->boundary0_center_y, info->boundary0_radius);
			apply_boundary_info(create_info.cams_info.views[1], info->boundary1_center_x, info->boundary1_center_y, info->boundary1_radius);
		} else {
			create_info.cams_info.views[0].boundary_type = HT_IMAGE_BOUNDARY_NONE;
			create_info.cams_info.views[1].boundary_type = HT_IMAGE_BOUNDARY_NONE;
		}
		create_info.cams_info.views[0].camera_orientation = orientation_from_int(info->orientation0);
		create_info.cams_info.views[1].camera_orientation = orientation_from_int(info->orientation1);

		ctx->sync = t_hand_tracking_sync_mercury_create(ctx->calib.get(), create_info, ctx->models_dir.c_str());
		if (!ctx->sync) throw std::runtime_error("t_hand_tracking_sync_mercury_create failed");
		return ctx;
	} catch (const std::exception &e) {
		ctx->last_error = e.what();
		delete ctx;
		return nullptr;
	}
}

extern "C" XR_MERCURY_RUNTIME_EXPORT void
xr_mercury_runtime_destroy(void *opaque)
{
	delete static_cast<RuntimeContext *>(opaque);
}

extern "C" XR_MERCURY_RUNTIME_EXPORT int
xr_mercury_runtime_process_gray8(void *opaque,
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
                                    struct xr_mercury_frame_f32 *out_frame)
{
	RuntimeContext *ctx = static_cast<RuntimeContext *>(opaque);
	if (!ctx || !out_frame) return -1;
	try {
		init_frame(*out_frame, source_timestamp_ns, sequence);

		struct xrt_frame *cam0_frame = make_frame_from_gray8(cam0, cam0_width, cam0_height, cam0_stride, source_timestamp_ns, sequence);
		struct xrt_frame *cam1_frame = make_frame_from_gray8(cam1, cam1_width, cam1_height, cam1_stride, source_timestamp_ns, sequence);
		struct xrt_frame *left_frame = ctx->swap_cameras ? cam1_frame : cam0_frame;
		struct xrt_frame *right_frame = ctx->swap_cameras ? cam0_frame : cam1_frame;

		struct xrt_hand_joint_set left_hand = {};
		struct xrt_hand_joint_set right_hand = {};
		int64_t out_timestamp_ns = 0;
		t_ht_sync_process(ctx->sync, left_frame, right_frame, &left_hand, &right_hand, &out_timestamp_ns);

		out_frame->timestamp_ns = (uint64_t)out_timestamp_ns;
		out_frame->source_timestamp_ns = source_timestamp_ns;
		out_frame->sequence = sequence;
		fill_side(out_frame->left, left_hand, 1);
		fill_side(out_frame->right, right_hand, 2);

		out_frame->hand_count = (left_hand.is_active ? 1u : 0u) + (right_hand.is_active ? 1u : 0u);
		out_frame->tracking_status = out_frame->hand_count > 0 ? HAND_STATUS_TRACKING : HAND_STATUS_NO_HANDS;
		out_frame->flags = 0;
		if (left_hand.is_active) out_frame->flags |= HAND_FLAG_LEFT_VALID | HAND_FLAG_JOINTS_VALID;
		if (right_hand.is_active) out_frame->flags |= HAND_FLAG_RIGHT_VALID | HAND_FLAG_JOINTS_VALID;
		out_frame->confidence = out_frame->hand_count > 0 ? 1.0f : 0.0f;

		xrt_frame_reference(&cam0_frame, nullptr);
		xrt_frame_reference(&cam1_frame, nullptr);
		return 0;
	} catch (const std::exception &e) {
		ctx->last_error = e.what();
		return -2;
	}
}

extern "C" XR_MERCURY_RUNTIME_EXPORT const char *
xr_mercury_runtime_last_error(void *opaque)
{
	RuntimeContext *ctx = static_cast<RuntimeContext *>(opaque);
	if (!ctx) return "null Mercury runtime context";
	return ctx->last_error.c_str();
}
