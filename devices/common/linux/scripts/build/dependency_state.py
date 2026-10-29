#!/usr/bin/env python3
"""Key the offline-ready marker by the selected build and its dependency inputs."""
import hashlib
import json
import os
from pathlib import Path
import sys


def key(root, command):
    # Include overrides of dependency refs, paths and toolchains, but exclude
    # run identities and the policy switches themselves. A new target/profile
    # gets its own first online build rather than borrowing an unrelated stamp.
    names = {
        "XR_BUILD_ONLY", "XR_BUILD_XRIZER", "XR_DOWNLOAD_HAND_TRACKING_MODELS",
        "XR_DOWNLOAD_MERCURY_MODELS", "XR_BUILD_VENDOR_COMPONENTS", "XR_VENDOR_COMPONENTS",
        "XR_TARGET_DEVICE", "XR_DEVICE_TARGET", "XR_DEVICE_OUT_ENV", "XR_DEVICE_BUILD_HOOK",
        "XR_USE_BUNDLED_OPENCV", "XR_CPU_CACHE_TAG", "CC", "CXX", "CFLAGS", "CXXFLAGS",
        "CXXFLAGS_EXTRA", "CMAKE_BIN", "CMAKE_PRESET", "CARGO_HOME", "RUSTUP_HOME",
        "RUSTUP_TOOLCHAIN", "THIRD_PARTY_DIR", "BUILD_TARGETS", "MONADO_EXTRA_CMAKE_ARGS",
    }
    settings = {name: value for name, value in os.environ.items()
                if name in names or name.startswith(("BASALT_", "MONADO_", "MERCURY_", "ORT_", "OPENVR_", "XRIZER_"))}
    digest = hashlib.sha256(json.dumps([command, settings], sort_keys=True).encode())
    digest.update(Path("/etc/os-release").read_bytes())
    # Changes in integration scripts, manifests or triplets can add downloads.
    # Exclude third_party checkouts that installers patch in place, and generated
    # output/cache directories that must not invalidate the next build.
    for directory in ("devices/common/linux/scripts/build", "backends", "drivers", "capture_service_cpp",
                      "apps", "bridges", "tools", "runtime_adapters", "override_controller", "shared"):
        for path in sorted((root / directory).rglob("*")):
            if path.is_file() and (path.suffix in (".sh", ".cmake", ".toml") or path.name in ("CMakeLists.txt", "vcpkg.json", "Cargo.lock")):
                digest.update(str(path.relative_to(root)).encode())
                digest.update(path.read_bytes())
    return digest.hexdigest()


if __name__ == "__main__":
    print(key(Path(sys.argv[1]), sys.argv[2:]))
