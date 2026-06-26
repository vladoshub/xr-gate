// Copyright 2026
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Offline Mercury probe for XR stereo hand datasets.
 *
 * This is intentionally a small development/debug tool. It reads the HT0
 * dataset layout produced by capture_hand_tracking_backend and calls Mercury's
 * synchronous hand tracking API on annotated stereo pairs.
 */

#include "hg_interface.h"

#include "tracking/t_hand_tracking.h"
#include "tracking/t_tracking.h"
#include "util/u_frame.h"
#include "xrt/xrt_defines.h"
#include "xrt/xrt_frame.h"

#include <cjson/cJSON.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;

namespace {

struct Args
{
	fs::path dataset;
	fs::path models;
	fs::path annotations;
	fs::path calib;
	fs::path output;
	bool swap_cameras = false;
	bool process_all = false;
	bool only_report_annotations = false;
	bool cam0_flip_horizontal = false;
	bool cam1_flip_horizontal = false;
	bool cam0_flip_vertical = false;
	bool cam1_flip_vertical = false;
	bool cam0_rotate_180 = false;
	bool cam1_rotate_180 = false;
	bool dump_key_joints = false;
	bool boundary_circle = false;
	float boundary0_center_x = 0.5f;
	float boundary0_center_y = 0.5f;
	float boundary0_radius = 0.55f;
	float boundary1_center_x = 0.5f;
	float boundary1_center_y = 0.5f;
	float boundary1_radius = 0.55f;
	int orientation0 = CAMERA_ORIENTATION_0;
	int orientation1 = CAMERA_ORIENTATION_0;
};

struct ImageTransform
{
	bool flip_horizontal = false;
	bool flip_vertical = false;
	bool rotate_180 = false;
};

struct CsvRow
{
	uint64_t dump_index = 0;
	std::string label;
	uint64_t pair_sequence = 0;
	int64_t pair_timestamp_ns = 0;
	fs::path cam0_path;
	fs::path cam1_path;
};

struct PgmImage
{
	uint32_t width = 0;
	uint32_t height = 0;
	std::vector<uint8_t> pixels;
};

struct QuatPose
{
	double px = 0.0, py = 0.0, pz = 0.0;
	double qx = 0.0, qy = 0.0, qz = 0.0, qw = 1.0;
};

struct Mat3
{
	double v[3][3]{};
};

struct Vec3
{
	double v[3]{};
};

static void
usage(const char *argv0)
{
	std::cerr << "Usage: " << argv0 << " \\\n"
	          << "  --dataset <HT0 dataset dir> \\\n"
	          << "  --models <Mercury model dir> \\\n"
	          << "  [--annotations <csv>] \\\n"
	          << "  [--calib <mercury_calib_unified_480_ccw90.json>] \\\n"
	          << "  [--output <out dir>] \\\n"
	          << "  [--swap-cameras] \\\n"
	          << "  [--process-all] [--only-report-annotations] \\\n"
	          << "  [--orientation0 0|90|180|270] [--orientation1 0|90|180|270] \\n"
	          << "  [--boundary-circle] \\n"
	          << "  [--boundary0-center-x <0..1>] [--boundary0-center-y <0..1>] [--boundary0-radius <r>] \\n"
	          << "  [--boundary1-center-x <0..1>] [--boundary1-center-y <0..1>] [--boundary1-radius <r>] \\n"
	          << "  [--dump-key-joints]\n";
}

static bool
parse_bool_flag(const std::string &s, const std::string &name)
{
	return s == name;
}

static Args
parse_args(int argc, char **argv)
{
	Args args;

	for (int i = 1; i < argc; i++) {
		std::string a = argv[i];
		auto need_value = [&](const char *name) -> std::string {
			if (i + 1 >= argc) {
				throw std::runtime_error(std::string("missing value for ") + name);
			}
			return argv[++i];
		};

		if (a == "--dataset") {
			args.dataset = need_value("--dataset");
		} else if (a == "--models") {
			args.models = need_value("--models");
		} else if (a == "--annotations") {
			args.annotations = need_value("--annotations");
		} else if (a == "--calib") {
			args.calib = need_value("--calib");
		} else if (a == "--output") {
			args.output = need_value("--output");
		} else if (parse_bool_flag(a, "--swap-cameras")) {
			args.swap_cameras = true;
		} else if (parse_bool_flag(a, "--process-all")) {
			args.process_all = true;
		} else if (parse_bool_flag(a, "--only-report-annotations")) {
			args.only_report_annotations = true;
		} else if (parse_bool_flag(a, "--dump-key-joints")) {
			args.dump_key_joints = true;
		} else if (parse_bool_flag(a, "--flip-horizontal")) {
			args.cam0_flip_horizontal = true;
			args.cam1_flip_horizontal = true;
		} else if (parse_bool_flag(a, "--flip-vertical")) {
			args.cam0_flip_vertical = true;
			args.cam1_flip_vertical = true;
		} else if (parse_bool_flag(a, "--rotate-180")) {
			args.cam0_rotate_180 = true;
			args.cam1_rotate_180 = true;
		} else if (parse_bool_flag(a, "--cam0-flip-horizontal")) {
			args.cam0_flip_horizontal = true;
		} else if (parse_bool_flag(a, "--cam1-flip-horizontal")) {
			args.cam1_flip_horizontal = true;
		} else if (parse_bool_flag(a, "--cam0-flip-vertical")) {
			args.cam0_flip_vertical = true;
		} else if (parse_bool_flag(a, "--cam1-flip-vertical")) {
			args.cam1_flip_vertical = true;
		} else if (parse_bool_flag(a, "--cam0-rotate-180")) {
			args.cam0_rotate_180 = true;
		} else if (parse_bool_flag(a, "--cam1-rotate-180")) {
			args.cam1_rotate_180 = true;
		} else if (parse_bool_flag(a, "--boundary-circle")) {
			args.boundary_circle = true;
		} else if (a == "--boundary0-center-x") {
			args.boundary0_center_x = std::stof(need_value("--boundary0-center-x"));
		} else if (a == "--boundary0-center-y") {
			args.boundary0_center_y = std::stof(need_value("--boundary0-center-y"));
		} else if (a == "--boundary0-radius") {
			args.boundary0_radius = std::stof(need_value("--boundary0-radius"));
		} else if (a == "--boundary1-center-x") {
			args.boundary1_center_x = std::stof(need_value("--boundary1-center-x"));
		} else if (a == "--boundary1-center-y") {
			args.boundary1_center_y = std::stof(need_value("--boundary1-center-y"));
		} else if (a == "--boundary1-radius") {
			args.boundary1_radius = std::stof(need_value("--boundary1-radius"));
		} else if (a == "--orientation0") {
			args.orientation0 = std::stoi(need_value("--orientation0"));
		} else if (a == "--orientation1") {
			args.orientation1 = std::stoi(need_value("--orientation1"));
		} else if (a == "--help" || a == "-h") {
			usage(argv[0]);
			exit(0);
		} else {
			throw std::runtime_error("unknown argument: " + a);
		}
	}

	if (args.dataset.empty()) {
		throw std::runtime_error("--dataset is required");
	}
	if (args.models.empty()) {
		throw std::runtime_error("--models is required");
	}
	if (args.annotations.empty()) {
		args.annotations = args.dataset / "annotations.csv";
	}
	if (args.calib.empty()) {
		args.calib = args.dataset / "calibration" / "mercury_calib_unified_480_ccw90.json";
	}
	if (args.output.empty()) {
		args.output = "/tmp/xr_mercury_probe";
	}

	auto check_boundary_value = [](float v, const char *name) {
		if (!std::isfinite(v)) {
			throw std::runtime_error(std::string("bad finite value for ") + name);
		}
	};
	check_boundary_value(args.boundary0_center_x, "--boundary0-center-x");
	check_boundary_value(args.boundary0_center_y, "--boundary0-center-y");
	check_boundary_value(args.boundary0_radius, "--boundary0-radius");
	check_boundary_value(args.boundary1_center_x, "--boundary1-center-x");
	check_boundary_value(args.boundary1_center_y, "--boundary1-center-y");
	check_boundary_value(args.boundary1_radius, "--boundary1-radius");
	if (args.boundary0_radius <= 0.0f || args.boundary1_radius <= 0.0f) {
		throw std::runtime_error("boundary radius must be positive");
	}

	return args;
}

static std::string
trim_cr(std::string s)
{
	while (!s.empty() && (s.back() == '\r' || s.back() == '\n')) {
		s.pop_back();
	}
	return s;
}

static std::vector<std::string>
split_csv_line(const std::string &line)
{
	// The HT0 CSV writer does not emit quoted fields, so a simple splitter is enough.
	std::vector<std::string> out;
	std::string cur;
	std::istringstream ss(trim_cr(line));
	while (std::getline(ss, cur, ',')) {
		out.push_back(trim_cr(cur));
	}
	return out;
}

static std::vector<CsvRow>
load_annotations(const fs::path &path)
{
	std::ifstream f(path);
	if (!f) {
		throw std::runtime_error("failed to open annotations: " + path.string());
	}

	std::string line;
	if (!std::getline(f, line)) {
		throw std::runtime_error("empty annotations: " + path.string());
	}

	std::vector<CsvRow> rows;
	while (std::getline(f, line)) {
		line = trim_cr(line);
		if (line.empty()) {
			continue;
		}
		auto c = split_csv_line(line);
		if (c.size() < 6) {
			throw std::runtime_error("bad annotations row: " + line);
		}

		CsvRow r;
		r.dump_index = std::stoull(c[0]);
		r.label = c[1];
		r.pair_sequence = std::stoull(c[2]);
		r.pair_timestamp_ns = std::stoll(c[3]);
		r.cam0_path = c[4];
		r.cam1_path = c[5];
		rows.push_back(std::move(r));
	}

	return rows;
}

static std::unordered_map<uint64_t, CsvRow>
make_annotation_map(const std::vector<CsvRow> &annotations)
{
	std::unordered_map<uint64_t, CsvRow> map;
	for (const CsvRow &row : annotations) {
		map[row.dump_index] = row;
	}
	return map;
}

static std::vector<CsvRow>
load_camera_timestamps(const fs::path &path, const std::unordered_map<uint64_t, CsvRow> &annotations)
{
	std::ifstream f(path);
	if (!f) {
		throw std::runtime_error("failed to open camera timestamps: " + path.string());
	}

	std::string line;
	if (!std::getline(f, line)) {
		throw std::runtime_error("empty camera timestamps: " + path.string());
	}

	std::vector<CsvRow> rows;
	while (std::getline(f, line)) {
		line = trim_cr(line);
		if (line.empty()) {
			continue;
		}
		auto c = split_csv_line(line);
		if (c.size() < 13) {
			throw std::runtime_error("bad camera timestamps row: " + line);
		}

		CsvRow r;
		r.dump_index = std::stoull(c[0]);
		r.label = "unlabeled";
		r.pair_sequence = std::stoull(c[1]);
		r.pair_timestamp_ns = std::stoll(c[2]);
		r.cam0_path = c[7];
		r.cam1_path = c[12];

		auto it = annotations.find(r.dump_index);
		if (it != annotations.end()) {
			r.label = it->second.label;
		}

		rows.push_back(std::move(r));
	}

	return rows;
}

static std::string
read_text_file(const fs::path &path)
{
	std::ifstream f(path, std::ios::binary);
	if (!f) {
		throw std::runtime_error("failed to open: " + path.string());
	}
	std::ostringstream ss;
	ss << f.rdbuf();
	return ss.str();
}

class JsonDoc
{
public:
	explicit JsonDoc(const fs::path &path)
	{
		std::string text = read_text_file(path);
		root_ = cJSON_Parse(text.c_str());
		if (!root_) {
			throw std::runtime_error("failed to parse JSON: " + path.string());
		}
	}

	~JsonDoc() { cJSON_Delete(root_); }

	cJSON *root() const { return root_; }

private:
	cJSON *root_ = nullptr;
};

static cJSON *
obj(cJSON *parent, const char *name)
{
	cJSON *v = cJSON_GetObjectItemCaseSensitive(parent, name);
	if (!v) {
		throw std::runtime_error(std::string("missing JSON object/item: ") + name);
	}
	return v;
}

static cJSON *
arr_item(cJSON *array, int idx, const char *name)
{
	if (!cJSON_IsArray(array)) {
		throw std::runtime_error(std::string("expected array: ") + name);
	}
	cJSON *v = cJSON_GetArrayItem(array, idx);
	if (!v) {
		throw std::runtime_error(std::string("missing array item: ") + name);
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
	x /= n;
	y /= n;
	z /= n;
	w /= n;

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

static Mat3
transpose(const Mat3 &a)
{
	Mat3 r;
	for (int i = 0; i < 3; i++) {
		for (int j = 0; j < 3; j++) {
			r.v[i][j] = a.v[j][i];
		}
	}
	return r;
}

static Mat3
mul(const Mat3 &a, const Mat3 &b)
{
	Mat3 r;
	for (int i = 0; i < 3; i++) {
		for (int j = 0; j < 3; j++) {
			r.v[i][j] = 0.0;
			for (int k = 0; k < 3; k++) {
				r.v[i][j] += a.v[i][k] * b.v[k][j];
			}
		}
	}
	return r;
}

static Vec3
mul(const Mat3 &a, const Vec3 &b)
{
	Vec3 r;
	for (int i = 0; i < 3; i++) {
		r.v[i] = a.v[i][0] * b.v[0] + a.v[i][1] * b.v[1] + a.v[i][2] * b.v[2];
	}
	return r;
}

static Vec3
sub(const Vec3 &a, const Vec3 &b)
{
	return Vec3{{a.v[0] - b.v[0], a.v[1] - b.v[1], a.v[2] - b.v[2]}};
}

static Vec3
pose_t(const QuatPose &p)
{
	return Vec3{{p.px, p.py, p.pz}};
}

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
	cJSON *value0 = obj(doc.root(), "value0");
	cJSON *intrinsics = obj(value0, "intrinsics");
	cJSON *resolution = obj(value0, "resolution");
	cJSON *t_imu_cam = obj(value0, "T_imu_cam");

	struct t_stereo_camera_calibration *calib = nullptr;
	t_stereo_camera_calibration_alloc(&calib, T_DISTORTION_FISHEYE_KB4);
	if (!calib) {
		throw std::runtime_error("t_stereo_camera_calibration_alloc failed");
	}

	fill_camera_calibration(calib->view[0], arr_item(intrinsics, 0, "intrinsics[0]"),
	                        arr_item(resolution, 0, "resolution[0]"));
	fill_camera_calibration(calib->view[1], arr_item(intrinsics, 1, "intrinsics[1]"),
	                        arr_item(resolution, 1, "resolution[1]"));

	QuatPose p0 = read_pose(arr_item(t_imu_cam, 0, "T_imu_cam[0]"));
	QuatPose p1 = read_pose(arr_item(t_imu_cam, 1, "T_imu_cam[1]"));
	Mat3 R_i_c0 = quat_to_mat(p0);
	Mat3 R_i_c1 = quat_to_mat(p1);
	Mat3 R_c1_i = transpose(R_i_c1);

	// Basalt stores T_imu_cam. Convert to OpenCV-style transform from first camera
	// to second camera: X_cam1 = R_cam1_cam0 * X_cam0 + t_cam1_cam0.
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

static PgmImage
load_pgm_p5(const fs::path &path)
{
	std::ifstream f(path, std::ios::binary);
	if (!f) {
		throw std::runtime_error("failed to open PGM: " + path.string());
	}
	std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
	if (data.empty()) {
		throw std::runtime_error("empty PGM: " + path.string());
	}

	size_t pos = 0;
	auto next_token = [&]() -> std::string {
		while (pos < data.size()) {
			uint8_t c = data[pos];
			if (std::isspace(c)) {
				pos++;
				continue;
			}
			if (c == '#') {
				while (pos < data.size() && data[pos] != '\n' && data[pos] != '\r') {
					pos++;
				}
				continue;
			}
			break;
		}
		size_t start = pos;
		while (pos < data.size() && !std::isspace(data[pos])) {
			pos++;
		}
		return std::string((const char *)&data[start], pos - start);
	};

	std::string magic = next_token();
	if (magic != "P5") {
		throw std::runtime_error("PGM is not P5: " + path.string());
	}
	uint32_t width = (uint32_t)std::stoul(next_token());
	uint32_t height = (uint32_t)std::stoul(next_token());
	int maxval = std::stoi(next_token());
	if (maxval != 255) {
		throw std::runtime_error("PGM maxval != 255: " + path.string());
	}
	// PGM P5 has a single whitespace delimiter after maxval. Do not skip
	// arbitrary whitespace here: binary pixel data itself may start with values
	// that are classified as whitespace.
	if (pos < data.size() && std::isspace(data[pos])) {
		pos++;
	}
	const size_t expected = (size_t)width * (size_t)height;
	if (data.size() - pos != expected) {
		throw std::runtime_error("PGM payload size mismatch: " + path.string());
	}

	PgmImage img;
	img.width = width;
	img.height = height;
	img.pixels.assign(data.begin() + (std::ptrdiff_t)pos, data.end());
	return img;
}

static void
apply_transform(PgmImage &img, const ImageTransform &transform)
{
	if (!(transform.flip_horizontal || transform.flip_vertical || transform.rotate_180)) {
		return;
	}

	std::vector<uint8_t> out(img.pixels.size());
	const uint32_t w = img.width;
	const uint32_t h = img.height;

	for (uint32_t y = 0; y < h; y++) {
		for (uint32_t x = 0; x < w; x++) {
			uint32_t sx = x;
			uint32_t sy = y;

			if (transform.rotate_180) {
				sx = w - 1 - sx;
				sy = h - 1 - sy;
			}
			if (transform.flip_horizontal) {
				sx = w - 1 - sx;
			}
			if (transform.flip_vertical) {
				sy = h - 1 - sy;
			}

			out[(size_t)y * w + x] = img.pixels[(size_t)sy * w + sx];
		}
	}

	img.pixels.swap(out);
}

static std::string
transform_to_string(const ImageTransform &t)
{
	std::ostringstream os;
	os << "fh=" << (t.flip_horizontal ? 1 : 0) << ",fv=" << (t.flip_vertical ? 1 : 0)
	   << ",r180=" << (t.rotate_180 ? 1 : 0);
	return os.str();
}

static struct xrt_frame *
make_frame_from_pgm(const fs::path &path, int64_t timestamp_ns, uint64_t sequence, const ImageTransform &transform)
{
	PgmImage img = load_pgm_p5(path);
	apply_transform(img, transform);

	struct xrt_frame *frame = nullptr;
	u_frame_create_one_off(XRT_FORMAT_L8, img.width, img.height, &frame);
	if (!frame) {
		throw std::runtime_error("u_frame_create_one_off failed");
	}

	for (uint32_t y = 0; y < img.height; y++) {
		std::memcpy(frame->data + y * frame->stride, img.pixels.data() + (size_t)y * img.width, img.width);
	}

	frame->timestamp = timestamp_ns;
	frame->source_timestamp = timestamp_ns;
	frame->source_sequence = sequence;
	frame->source_id = 0;
	return frame;
}

static void
write_joint_summary(std::ostream &os, const xrt_hand_joint_set &hand, enum xrt_hand_joint joint)
{
	const auto &j = hand.values.hand_joint_set_default[joint];
	const auto flags = (uint32_t)j.relation.relation_flags;
	const auto &p = j.relation.pose.position;
	os << flags << ',' << p.x << ',' << p.y << ',' << p.z;
}


struct NamedJoint
{
	const char *name;
	enum xrt_hand_joint joint;
};

static const std::array<NamedJoint, 10> KEY_JOINTS = {{
    {"palm", XRT_HAND_JOINT_PALM},
    {"wrist", XRT_HAND_JOINT_WRIST},
    {"thumb_tip", XRT_HAND_JOINT_THUMB_TIP},
    {"index_metacarpal", XRT_HAND_JOINT_INDEX_METACARPAL},
    {"index_tip", XRT_HAND_JOINT_INDEX_TIP},
    {"middle_tip", XRT_HAND_JOINT_MIDDLE_TIP},
    {"ring_tip", XRT_HAND_JOINT_RING_TIP},
    {"little_metacarpal", XRT_HAND_JOINT_LITTLE_METACARPAL},
    {"little_tip", XRT_HAND_JOINT_LITTLE_TIP},
    {"thumb_metacarpal", XRT_HAND_JOINT_THUMB_METACARPAL},
}};

static void
write_key_joint_header(std::ostream &os, const char *prefix)
{
	for (const auto &nj : KEY_JOINTS) {
		os << ',' << prefix << '_' << nj.name << "_flags";
		os << ',' << prefix << '_' << nj.name << "_x";
		os << ',' << prefix << '_' << nj.name << "_y";
		os << ',' << prefix << '_' << nj.name << "_z";
	}
}

static void
write_key_joint_values(std::ostream &os, const xrt_hand_joint_set &hand)
{
	for (const auto &nj : KEY_JOINTS) {
		os << ',';
		write_joint_summary(os, hand, nj.joint);
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

static enum t_camera_orientation
to_orientation(int v)
{
	switch (v) {
	case 0: return CAMERA_ORIENTATION_0;
	case 90: return CAMERA_ORIENTATION_90;
	case 180: return CAMERA_ORIENTATION_180;
	case 270: return CAMERA_ORIENTATION_270;
	default: throw std::runtime_error("bad camera orientation, expected 0/90/180/270");
	}
}

} // namespace

int
main(int argc, char **argv)
{
	try {
		Args args = parse_args(argc, argv);

		if (!fs::exists(args.dataset)) {
			throw std::runtime_error("dataset does not exist: " + args.dataset.string());
		}
		if (!fs::exists(args.models)) {
			throw std::runtime_error("models dir does not exist: " + args.models.string());
		}
		if (!fs::exists(args.models / "grayscale_detection_160x160.onnx") ||
		    !fs::exists(args.models / "grayscale_keypoint_jan18.onnx")) {
			throw std::runtime_error("models dir does not contain Mercury grayscale ONNX files: " + args.models.string());
		}

		fs::create_directories(args.output);

		std::vector<CsvRow> annotations = load_annotations(args.annotations);
		auto annotation_map = make_annotation_map(annotations);
		std::vector<CsvRow> rows = args.process_all
		                           ? load_camera_timestamps(args.dataset / "camera_timestamps.csv", annotation_map)
		                           : annotations;
		std::cerr << "Loaded annotations: " << annotations.size() << "\n";
		std::cerr << "Processing rows: " << rows.size() << "\n";

		struct t_stereo_camera_calibration *calib = load_stereo_calibration_json(args.calib);

		struct t_hand_tracking_create_info create_info = {};
		create_info.masks_sink = nullptr;
		if (args.boundary_circle) {
			apply_boundary_info(create_info.cams_info.views[0], args.boundary0_center_x, args.boundary0_center_y,
			                    args.boundary0_radius);
			apply_boundary_info(create_info.cams_info.views[1], args.boundary1_center_x, args.boundary1_center_y,
			                    args.boundary1_radius);
		} else {
			create_info.cams_info.views[0].boundary_type = HT_IMAGE_BOUNDARY_NONE;
			create_info.cams_info.views[1].boundary_type = HT_IMAGE_BOUNDARY_NONE;
		}
		create_info.cams_info.views[0].camera_orientation = to_orientation(args.orientation0);
		create_info.cams_info.views[1].camera_orientation = to_orientation(args.orientation1);

		struct t_hand_tracking_sync *sync =
		    t_hand_tracking_sync_mercury_create(calib, create_info, args.models.string().c_str());
		if (!sync) {
			t_stereo_camera_calibration_reference(&calib, nullptr);
			throw std::runtime_error("t_hand_tracking_sync_mercury_create failed");
		}

		fs::path csv_path = args.output / "results.csv";
		std::ofstream out(csv_path);
		if (!out) {
			throw std::runtime_error("failed to create output CSV: " + csv_path.string());
		}

		out << "dump_index,label,pair_sequence,pair_timestamp_ns,"
		       "left_active,right_active,out_timestamp_ns,"
		       "left_palm_flags,left_palm_x,left_palm_y,left_palm_z,"
		       "left_wrist_flags,left_wrist_x,left_wrist_y,left_wrist_z,"
		       "right_palm_flags,right_palm_x,right_palm_y,right_palm_z,"
		       "right_wrist_flags,right_wrist_x,right_wrist_y,right_wrist_z";
		if (args.dump_key_joints) {
			write_key_joint_header(out, "left");
			write_key_joint_header(out, "right");
		}
		out << "\n";

		size_t processed = 0;
		size_t left_active_count = 0;
		size_t right_active_count = 0;
		size_t reported = 0;

		const ImageTransform cam0_transform{args.cam0_flip_horizontal, args.cam0_flip_vertical, args.cam0_rotate_180};
		const ImageTransform cam1_transform{args.cam1_flip_horizontal, args.cam1_flip_vertical, args.cam1_rotate_180};

		for (const CsvRow &row : rows) {
			// Feed dataset metadata to Mercury debug dump filters/file names.
			// This lets hg_sync.cpp implement dump_index-centered debug windows without changing the
			// t_hand_tracking_sync API. setenv copies the strings immediately.
			const std::string xr_current_dump_index = std::to_string(row.dump_index);
			setenv("MERCURY_XR_CURRENT_DUMP_INDEX", xr_current_dump_index.c_str(), 1);
			setenv("MERCURY_XR_CURRENT_LABEL", row.label.c_str(), 1);

			fs::path cam0 = args.dataset / row.cam0_path;
			fs::path cam1 = args.dataset / row.cam1_path;

			struct xrt_frame *cam0_frame = make_frame_from_pgm(cam0, row.pair_timestamp_ns, row.pair_sequence, cam0_transform);
			struct xrt_frame *cam1_frame = make_frame_from_pgm(cam1, row.pair_timestamp_ns, row.pair_sequence, cam1_transform);
			struct xrt_frame *left_frame = args.swap_cameras ? cam1_frame : cam0_frame;
			struct xrt_frame *right_frame = args.swap_cameras ? cam0_frame : cam1_frame;

			struct xrt_hand_joint_set left_hand = {};
			struct xrt_hand_joint_set right_hand = {};
			int64_t out_timestamp_ns = 0;

			t_ht_sync_process(sync, left_frame, right_frame, &left_hand, &right_hand, &out_timestamp_ns);

			const bool is_annotated = annotation_map.find(row.dump_index) != annotation_map.end();
			const bool should_report = !(args.only_report_annotations && !is_annotated);
			if (should_report) {
				reported++;
				if (left_hand.is_active) {
					left_active_count++;
				}
				if (right_hand.is_active) {
					right_active_count++;
				}
				out << row.dump_index << ',' << row.label << ',' << row.pair_sequence << ',' << row.pair_timestamp_ns << ','
				    << (left_hand.is_active ? 1 : 0) << ',' << (right_hand.is_active ? 1 : 0) << ','
				    << out_timestamp_ns << ',';
				write_joint_summary(out, left_hand, XRT_HAND_JOINT_PALM);
				out << ',';
				write_joint_summary(out, left_hand, XRT_HAND_JOINT_WRIST);
				out << ',';
				write_joint_summary(out, right_hand, XRT_HAND_JOINT_PALM);
				out << ',';
				write_joint_summary(out, right_hand, XRT_HAND_JOINT_WRIST);
				if (args.dump_key_joints) {
					write_key_joint_values(out, left_hand);
					write_key_joint_values(out, right_hand);
				}
				out << "\n";
			}

			xrt_frame_reference(&cam0_frame, nullptr);
			xrt_frame_reference(&cam1_frame, nullptr);
			processed++;
		}

		t_ht_sync_destroy(&sync);
		t_stereo_camera_calibration_reference(&calib, nullptr);

		std::ofstream summary(args.output / "summary.txt");
		summary << "processed=" << processed << "\n";
		summary << "reported=" << reported << "\n";
		summary << "left_active=" << left_active_count << "\n";
		summary << "right_active=" << right_active_count << "\n";
		summary << "process_all=" << (args.process_all ? 1 : 0) << "\n";
		summary << "only_report_annotations=" << (args.only_report_annotations ? 1 : 0) << "\n";
		summary << "swap_cameras=" << (args.swap_cameras ? 1 : 0) << "\n";
		summary << "orientation0=" << args.orientation0 << "\n";
		summary << "orientation1=" << args.orientation1 << "\n";
		summary << "boundary_circle=" << (args.boundary_circle ? 1 : 0) << "\n";
		if (args.boundary_circle) {
			summary << "boundary0=center=(" << args.boundary0_center_x << "," << args.boundary0_center_y
			        << "),radius=" << args.boundary0_radius << "\n";
			summary << "boundary1=center=(" << args.boundary1_center_x << "," << args.boundary1_center_y
			        << "),radius=" << args.boundary1_radius << "\n";
		}
		summary << "dump_key_joints=" << (args.dump_key_joints ? 1 : 0) << "\n";
		summary << "cam0_transform=" << transform_to_string(cam0_transform) << "\n";
		summary << "cam1_transform=" << transform_to_string(cam1_transform) << "\n";
		summary << "models=" << args.models << "\n";
		summary << "dataset=" << args.dataset << "\n";

		std::cout << "Wrote " << csv_path << "\n";
		std::cout << "processed=" << processed << " reported=" << reported << " left_active=" << left_active_count
		          << " right_active=" << right_active_count << "\n";
		return 0;
	} catch (const std::exception &e) {
		std::cerr << "xr_mercury_dataset_probe: error: " << e.what() << "\n";
		usage(argv[0]);
		return 1;
	}
}
