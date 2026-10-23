#pragma once
#include <stdexcept>
#include <string>
#include <xr_spatial/contracts/runtime_spatial_proxy_mesh_contract.hpp>
namespace xr_spatial {
inline bool mesh_layout_valid(const RuntimeSpatialProxyMeshF32V1 &m) {
  return m.version == RUNTIME_SPATIAL_PROXY_MESH_FORMAT_VERSION &&
         m.size_bytes == sizeof(m) &&
         m.vertex_count <= RUNTIME_SPATIAL_PROXY_MESH_MAX_VERTICES &&
         m.triangle_count <= RUNTIME_SPATIAL_PROXY_MESH_MAX_TRIANGLES;
}
inline std::string mesh_hex(const RuntimeSpatialProxyMeshF32V1 &m) {
  static constexpr char digits[] = "0123456789abcdef";
  const auto *bytes = reinterpret_cast<const unsigned char *>(&m);
  std::string out(sizeof(m) * 2, '0');
  for (size_t i = 0; i < sizeof(m); ++i) {
    out[i * 2] = digits[bytes[i] >> 4];
    out[i * 2 + 1] = digits[bytes[i] & 15];
  }
  return out;
}
inline RuntimeSpatialProxyMeshF32V1 mesh_from_hex(const std::string &text) {
  RuntimeSpatialProxyMeshF32V1 m;
  if (text.size() != sizeof(m) * 2)
    throw std::runtime_error("invalid spatial mesh byte count");
  auto digit = [](char c) -> unsigned {
    if (c >= '0' && c <= '9')
      return c - '0';
    if (c >= 'a' && c <= 'f')
      return c - 'a' + 10;
    throw std::runtime_error("invalid spatial mesh hex");
  };
  auto *bytes = reinterpret_cast<unsigned char *>(&m);
  for (size_t i = 0; i < sizeof(m); ++i)
    bytes[i] = static_cast<unsigned char>((digit(text[i * 2]) << 4) |
                                          digit(text[i * 2 + 1]));
  if (!mesh_layout_valid(m))
    throw std::runtime_error("invalid spatial mesh layout");
  return m;
}
} // namespace xr_spatial
