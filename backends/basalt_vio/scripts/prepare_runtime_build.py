#!/usr/bin/env python3
"""Reduce the pinned upstream build to the library and XR Gate's two tools."""
import json
import sys
from pathlib import Path


def prepare(root: Path, bundled_opencv: bool) -> None:
    cmake = root / "CMakeLists.txt"
    text = cmake.read_text()
    marker = "# ---- XR Tracking external backends ----"
    if marker in text:
        upstream = text.split(marker, 1)[0]
    elif "XR_TRACKING_ROOT" in text and "backends/basalt_vio" in text:
        # The optional historical .patch inserts integration between GUI tools.
        upstream = text
    else:
        raise SystemExit("Missing XR Tracking integration; refusing to trim upstream CMake")
    integration = '''
set(XR_TRACKING_ROOT "" CACHE PATH "Path to xr_tracking repository")
if(XR_TRACKING_ROOT)
  add_subdirectory(
    "${XR_TRACKING_ROOT}/backends/basalt_vio"
    "${CMAKE_BINARY_DIR}/xr_tracking/backends/basalt_vio"
  )
endif()
'''
    gui_start = "add_executable(basalt_calibrate src/calibrate.cpp"
    if gui_start in upstream:
        upstream = upstream[:upstream.index(gui_start)]
    elif "# XR_GATE_RUNTIME_ONLY" not in upstream:
        raise SystemExit("Unrecognized Basalt CMake layout; expected the pinned upstream revision")
    upstream = upstream.replace("# XR_GATE_RUNTIME_ONLY\n", "")
    for package in ("Pangolin", "GTest"):
        upstream = upstream.replace(f"find_package({package} CONFIG REQUIRED)", "")
    upstream = upstream.replace("enable_testing()", "")
    # Keep the original optimization and Eigen behavior, omit DWARF generation.
    upstream = upstream.replace('"-O3 -g -DEIGEN_INITIALIZE_MATRICES_BY_NAN"',
                                '"-O3 -g0 -DEIGEN_INITIALIZE_MATRICES_BY_NAN"')
    cmake.write_text(upstream.rstrip() + "\n\n# XR_GATE_RUNTIME_ONLY\n" + marker + integration)
    manifest = root / "vcpkg.json"
    data = json.loads(manifest.read_text())
    excluded = {"realsense2", "pangolin", "gtest"}
    if bundled_opencv:
        excluded.add("opencv4")
    data["dependencies"] = [dep for dep in data["dependencies"]
                            if (dep if isinstance(dep, str) else dep["name"]) not in excluded]
    manifest.write_text(json.dumps(data, indent=2) + "\n")
    # opengv's own add_definitions(-march=native) comes after the triplet flags.
    # Fix that port too; changing only Basalt's CXX_MARCH is insufficient.
    port = root / "vcpkg/ports/opengv/portfile.cmake"
    port_text = port.read_text()
    port_marker = "# XR_GATE_AVX2_OPENGV"
    if port_marker not in port_text:
        anchor = "vcpkg_cmake_configure("
        if anchor not in port_text:
            raise SystemExit("Unrecognized opengv overlay port")
        fix = '''# XR_GATE_AVX2_OPENGV
if(VCPKG_TARGET_IS_LINUX AND VCPKG_TARGET_ARCHITECTURE STREQUAL "x64")
    vcpkg_replace_string("${SOURCE_PATH}/CMakeLists.txt" "-march=native" "-march=x86-64-v3")
endif()

'''
        port.write_text(port_text.replace(anchor, fix + anchor, 1))


if __name__ == "__main__":
    prepare(Path(sys.argv[1]), sys.argv[2] == "1")
