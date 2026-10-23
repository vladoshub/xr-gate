// Experimental project extension: not a Khronos-registered vendor extension.
#pragma once
#include <openxr/openxr.h>
#define XR_MNDX_xr_gate_data 1
#define XR_MNDX_xr_gate_data_SPEC_VERSION 1
#define XR_MNDX_XR_GATE_DATA_EXTENSION_NAME "XR_MNDX_xr_gate_data"
#define XR_TYPE_DATA_SNAPSHOT_CREATE_INFO_MNDX ((XrStructureType)0x7f580001)
#define XR_TYPE_DATA_SNAPSHOT_PROPERTIES_MNDX ((XrStructureType)0x7f580002)
XR_DEFINE_HANDLE(XrDataSnapshotMNDX)
typedef struct XrDataSnapshotCreateInfoMNDX {
  XrStructureType type;
  const void *next;
  XrSpace baseSpace; // Required: transform from runtime coordinates is returned
                     // in properties.
  XrTime time;
} XrDataSnapshotCreateInfoMNDX;
typedef struct XrDataSnapshotPropertiesMNDX {
  XrStructureType type;
  void *next;
  XrBool32 spatialAvailable;
  XrBool32 videoAvailable;
  XrBool32 spatialTrackingValid;
  uint64_t trackingEpoch;
  XrPosef runtimeToBase;
  // These are source monotonic nanoseconds, NOT XrTime. Preserve for source
  // synchronization.
  uint64_t spatialSourceTimestampNs;
  uint64_t videoSourceTimestampNs;
} XrDataSnapshotPropertiesMNDX;
typedef XrResult(XRAPI_PTR *PFN_xrCreateDataSnapshotMNDX)(
    XrSession, const XrDataSnapshotCreateInfoMNDX *, XrDataSnapshotMNDX *);
typedef XrResult(XRAPI_PTR *PFN_xrGetDataSnapshotPropertiesMNDX)(
    XrDataSnapshotMNDX, XrDataSnapshotPropertiesMNDX *);
// Spatial bytes = exact RUNTIME_SPATIAL_PROXY_MESH_F32_V2 payload. Video bytes
// = XR_STEREO_VIDEO_V1 header, left bytes, right bytes, unchanged. CPU copies,
// bounded by the snapshot lifetime. Query capacity=0 first. All getters are
// read-only.
typedef XrResult(XRAPI_PTR *PFN_xrGetSpatialMeshMNDX)(XrDataSnapshotMNDX,
                                                      uint32_t, uint32_t *,
                                                      uint8_t *);
typedef XrResult(XRAPI_PTR *PFN_xrGetStereoVideoMNDX)(XrDataSnapshotMNDX,
                                                      uint32_t, uint32_t *,
                                                      uint8_t *);
typedef XrResult(XRAPI_PTR *PFN_xrDestroyDataSnapshotMNDX)(XrDataSnapshotMNDX);
