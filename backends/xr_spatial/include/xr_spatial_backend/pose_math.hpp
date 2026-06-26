#pragma once

#include <array>
#include <cmath>
#include <stdexcept>

#include <xr_spatial_backend/types.hpp>

namespace xr_spatial_backend {

struct Mat3d {
  double v[3][3] = {{1.0, 0.0, 0.0}, {0.0, 1.0, 0.0}, {0.0, 0.0, 1.0}};
};

struct Transform3d {
  Mat3d R;
  Vec3d t;
};

inline Quatd normalize(Quatd q) {
  const double n = std::sqrt(q.w*q.w + q.x*q.x + q.y*q.y + q.z*q.z);
  if (n <= 0.0 || !std::isfinite(n)) throw std::runtime_error("invalid zero/non-finite quaternion");
  q.w /= n; q.x /= n; q.y /= n; q.z /= n;
  return q;
}

inline Mat3d quat_to_mat(Quatd q) {
  q = normalize(q);
  const double w = q.w, x = q.x, y = q.y, z = q.z;
  Mat3d r;
  r.v[0][0] = 1.0 - 2.0 * (y*y + z*z);
  r.v[0][1] = 2.0 * (x*y - z*w);
  r.v[0][2] = 2.0 * (x*z + y*w);
  r.v[1][0] = 2.0 * (x*y + z*w);
  r.v[1][1] = 1.0 - 2.0 * (x*x + z*z);
  r.v[1][2] = 2.0 * (y*z - x*w);
  r.v[2][0] = 2.0 * (x*z - y*w);
  r.v[2][1] = 2.0 * (y*z + x*w);
  r.v[2][2] = 1.0 - 2.0 * (x*x + y*y);
  return r;
}

inline Mat3d transpose(const Mat3d& a) {
  Mat3d r;
  for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) r.v[i][j] = a.v[j][i];
  return r;
}

inline Mat3d multiply(const Mat3d& a, const Mat3d& b) {
  Mat3d r{};
  for (int i = 0; i < 3; ++i) {
    for (int j = 0; j < 3; ++j) {
      r.v[i][j] = 0.0;
      for (int k = 0; k < 3; ++k) r.v[i][j] += a.v[i][k] * b.v[k][j];
    }
  }
  return r;
}

inline Vec3d multiply(const Mat3d& a, const Vec3d& b) {
  return {
      a.v[0][0]*b.x + a.v[0][1]*b.y + a.v[0][2]*b.z,
      a.v[1][0]*b.x + a.v[1][1]*b.y + a.v[1][2]*b.z,
      a.v[2][0]*b.x + a.v[2][1]*b.y + a.v[2][2]*b.z,
  };
}

inline Vec3d operator+(const Vec3d& a, const Vec3d& b) { return {a.x+b.x, a.y+b.y, a.z+b.z}; }
inline Vec3d operator-(const Vec3d& a, const Vec3d& b) { return {a.x-b.x, a.y-b.y, a.z-b.z}; }

inline Transform3d compose(const Transform3d& a, const Transform3d& b) {
  Transform3d out;
  out.R = multiply(a.R, b.R);
  out.t = multiply(a.R, b.t) + a.t;
  return out;
}

inline Transform3d inverse(const Transform3d& T) {
  Transform3d out;
  out.R = transpose(T.R);
  const Vec3d neg_t{-T.t.x, -T.t.y, -T.t.z};
  out.t = multiply(out.R, neg_t);
  return out;
}

inline bool is_finite(const Vec3f& p) {
  return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z);
}

inline bool within_abs_limit(const Vec3f& p, float limit) {
  if (limit <= 0.0f) return true;
  return std::fabs(p.x) <= limit && std::fabs(p.y) <= limit && std::fabs(p.z) <= limit;
}

inline Vec3f transform_point(const Transform3d& T, const Vec3f& p) {
  const Vec3d d{p.x, p.y, p.z};
  const Vec3d r = multiply(T.R, d) + T.t;
  return {static_cast<float>(r.x), static_cast<float>(r.y), static_cast<float>(r.z)};
}

inline Transform3d pose_to_transform(const Pose3d& p) {
  return {quat_to_mat(p.q), p.p};
}

inline Transform3d pose_to_transform(double px, double py, double pz,
                                     double qw, double qx, double qy, double qz) {
  Pose3d p;
  p.p = {px, py, pz};
  p.q = {qw, qx, qy, qz};
  return pose_to_transform(p);
}

inline double distance(const Vec3f& a, const Vec3f& b) {
  const double dx = double(a.x) - double(b.x);
  const double dy = double(a.y) - double(b.y);
  const double dz = double(a.z) - double(b.z);
  return std::sqrt(dx*dx + dy*dy + dz*dz);
}

}  // namespace xr_spatial_backend
