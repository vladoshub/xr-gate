// Copyright 2023-2024, Collabora, Ltd.
// Copyright 2026, NVIDIA CORPORATION.
// SPDX-License-Identifier: BSL-1.0
//
// Device-space subset of Monado's XR_MNDX_xdev_space preview header.
// The system OpenXR loader can resolve these entry points even when its
// installed SDK lacks the preview declarations. See LICENSES/BSL-1.0.txt.
#pragma once

#include <openxr/openxr.h>

#ifndef XR_MNDX_xdev_space
#define XR_MNDX_XDEV_SPACE_EXTENSION_NAME "XR_MNDX_xdev_space"

XR_DEFINE_ATOM(XrXDevIdMNDX)
XR_DEFINE_HANDLE(XrXDevListMNDX)

inline constexpr auto XR_TYPE_SYSTEM_XDEV_SPACE_PROPERTIES_MNDX =
    static_cast<XrStructureType>(1000444001);
inline constexpr auto XR_TYPE_CREATE_XDEV_LIST_INFO_MNDX =
    static_cast<XrStructureType>(1000444002);
inline constexpr auto XR_TYPE_GET_XDEV_INFO_MNDX =
    static_cast<XrStructureType>(1000444003);
inline constexpr auto XR_TYPE_XDEV_PROPERTIES_MNDX =
    static_cast<XrStructureType>(1000444004);
inline constexpr auto XR_TYPE_CREATE_XDEV_SPACE_INFO_MNDX =
    static_cast<XrStructureType>(1000444005);

struct XrSystemXDevSpacePropertiesMNDX {
  XrStructureType type;
  void *next;
  XrBool32 supportsXDevSpace;
};

struct XrCreateXDevListInfoMNDX {
  XrStructureType type;
  const void *next;
};

struct XrGetXDevInfoMNDX {
  XrStructureType type;
  const void *next;
  XrXDevIdMNDX id;
};

struct XrXDevPropertiesMNDX {
  XrStructureType type;
  void *next;
  char name[256];
  char serial[256];
  XrBool32 canCreateSpace;
};

struct XrCreateXDevSpaceInfoMNDX {
  XrStructureType type;
  const void *next;
  XrXDevListMNDX xdevList;
  XrXDevIdMNDX id;
  XrPosef offset;
};

using PFN_xrCreateXDevListMNDX = XrResult(XRAPI_PTR *)(
    XrSession, const XrCreateXDevListInfoMNDX *, XrXDevListMNDX *);
using PFN_xrDestroyXDevListMNDX = XrResult(XRAPI_PTR *)(XrXDevListMNDX);
using PFN_xrEnumerateXDevsMNDX = XrResult(XRAPI_PTR *)(
    XrXDevListMNDX, uint32_t, uint32_t *, XrXDevIdMNDX *);
using PFN_xrGetXDevPropertiesMNDX = XrResult(XRAPI_PTR *)(
    XrXDevListMNDX, const XrGetXDevInfoMNDX *, XrXDevPropertiesMNDX *);
using PFN_xrCreateXDevSpaceMNDX = XrResult(XRAPI_PTR *)(
    XrSession, const XrCreateXDevSpaceInfoMNDX *, XrSpace *);
#endif
