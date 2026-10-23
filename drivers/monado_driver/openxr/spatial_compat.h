// Copyright (c) 2017-2026, The Khronos Group Inc.
// SPDX-License-Identifier: MIT
// Supplemental declarations from OpenXR-SDK openxr.h, for the pinned Monado
// SDK. No optional normals/semantic/watertight features are advertised by XR
// Gate.
#pragma once
#include <openxr/openxr.h>
#ifndef XR_EXT_spatial_mesh_tracking
#define XR_EXT_spatial_mesh_tracking 1
#define XR_EXT_spatial_mesh_tracking_SPEC_VERSION 1
#define XR_EXT_SPATIAL_MESH_TRACKING_EXTENSION_NAME                            \
  "XR_EXT_spatial_mesh_tracking"
#define XR_SPATIAL_CAPABILITY_MESH_TRACKING_EXT                                \
  ((XrSpatialCapabilityEXT)1000783000)
#define XR_TYPE_SPATIAL_CAPABILITY_CONFIGURATION_MESH_TRACKING_EXT             \
  ((XrStructureType)1000783000)
#define XR_TYPE_SPATIAL_FEATURE_TRACK_WATERTIGHT_MESH_EXT                      \
  ((XrStructureType)1000783001)
typedef struct XrSpatialCapabilityConfigurationMeshTrackingEXT {
  XrStructureType type;
  const void *XR_MAY_ALIAS next;
  XrSpatialCapabilityEXT capability;
  uint32_t enabledComponentCount;
  const XrSpatialComponentTypeEXT *enabledComponents;
} XrSpatialCapabilityConfigurationMeshTrackingEXT;

// XrSpatialFeatureTrackWatertightMeshEXT extends
// XrSpatialCapabilityConfigurationMeshTrackingEXT
typedef struct XrSpatialFeatureTrackWatertightMeshEXT {
  XrStructureType type;
  const void *XR_MAY_ALIAS next;
  XrBool32 enableWatertightMeshTracking;
} XrSpatialFeatureTrackWatertightMeshEXT;

#endif

#ifndef XR_EXT_spatial_entity_archetypes
// XR_EXT_spatial_entity_archetypes is a preprocessor guard. Do not pass it to
// API calls.
#define XR_EXT_spatial_entity_archetypes 1

#define XR_NULL_SPATIAL_ARCHETYPE_ID_EXT 0

XR_DEFINE_ATOM(XrSpatialArchetypeIdEXT)
#define XR_EXT_spatial_entity_archetypes_SPEC_VERSION 1
#define XR_EXT_SPATIAL_ENTITY_ARCHETYPES_EXTENSION_NAME                        \
  "XR_EXT_spatial_entity_archetypes"
// XrSpatialFilterArchetypeEXT extends XrSpatialComponentDataQueryConditionEXT
typedef struct XrSpatialFilterArchetypeEXT {
  XrStructureType type;
  const void *XR_MAY_ALIAS next;
  XrSpatialArchetypeIdEXT archetype;
} XrSpatialFilterArchetypeEXT;

typedef XrResult(XRAPI_PTR *PFN_xrEnumerateSpatialArchetypesEXT)(
    XrSpatialSnapshotEXT snapshot, uint32_t archetypeCapacityInput,
    uint32_t *archetypeCountOutput, XrSpatialArchetypeIdEXT *archetypes);
typedef XrResult(XRAPI_PTR *PFN_xrEnumerateSpatialArchetypeComponentTypesEXT)(
    XrSpatialSnapshotEXT snapshot, XrSpatialArchetypeIdEXT archetype,
    uint32_t componentTypeCapacityInput, uint32_t *componentTypeCountOutput,
    XrSpatialComponentTypeEXT *componentTypes);

#define XR_TYPE_SPATIAL_FILTER_ARCHETYPE_EXT ((XrStructureType)1000842000)
#endif
