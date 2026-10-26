#!/usr/bin/env python3
"""Describe Linux x86-64 build profiles and check a packaged CPU requirement."""
import argparse
import hashlib
import json
from pathlib import Path
import re


V1_FLAGS = {"cmov", "cx8", "fpu", "fxsr", "lm", "mmx", "sse", "sse2", "syscall"}
V3_FLAGS = V1_FLAGS | {"avx", "avx2", "fma", "f16c", "bmi1", "bmi2", "lzcnt", "movbe", "xsave",
            "cx16", "lahf_lm", "popcnt", "sse3", "ssse3", "sse4_1", "sse4_2"}
V4_FLAGS = V3_FLAGS | {"avx512f", "avx512bw", "avx512cd", "avx512dq", "avx512vl"}
PROFILES = {
    "generic": ("x86-64", "generic", "SSE2"),
    "avx2": ("x86-64-v3", "generic", "AVX2"),
    "avx512": ("x86-64-v4", "generic", "AVX512_SKX"),
    "native": ("native", "native", "NATIVE"),
}
SCHEMA = "xr_gate_cpu_profile_v1"


def normalize_flags(flags):
    result = set(flags)
    if "abm" in result:
        result.add("lzcnt")
    if "pni" in result:
        result.add("sse3")
    return result


def host_features(cpuinfo=None):
    text = Path("/proc/cpuinfo").read_text() if cpuinfo is None else cpuinfo
    cpus = re.findall(r"^flags\s*:\s*(.*)$", text, re.M)
    if not cpus:
        raise ValueError("Cannot determine x86 CPU features from /proc/cpuinfo")
    # Every schedulable CPU must support the requirement.
    return set.intersection(*(normalize_flags(cpu.split()) for cpu in cpus))


def describe(mode, cpuinfo=None):
    mode = mode.strip().lower()
    if mode not in PROFILES:
        raise ValueError("XR_BUILD_CPU must be generic, avx2, avx512 or native")
    march, tune, baseline = PROFILES[mode]
    if mode == "native":
        text = Path("/proc/cpuinfo").read_text() if cpuinfo is None else cpuinfo
        features = host_features(text)
        identity = re.findall(r"^(?:vendor_id|cpu family|model|stepping|model name)\s*:\s*(.*)$", text, re.M)
        identity = "\n".join(sorted(set(identity)) + sorted(features))
        cache_tag = "native-" + hashlib.sha256(identity.encode()).hexdigest()[:24]
    else:
        features = {"generic": V1_FLAGS, "avx2": V3_FLAGS, "avx512": V4_FLAGS}[mode]
        cache_tag = march
    return dict(schema=SCHEMA, profile=mode, march=march, tune=tune,
                opencv_baseline=baseline, cache_tag=cache_tag,
                required_features=sorted(features))


def check_package(root, cpuinfo=None):
    root = Path(root)
    marker = root / "BUILD_CPU_TARGET"
    if not marker.is_file():
        return
    march = marker.read_text().strip()
    if march == "x86-64":
        required = V1_FLAGS
    elif march == "x86-64-v3":
        required = V3_FLAGS
    elif march == "x86-64-v4":
        required = V4_FLAGS
    elif march == "native":
        metadata = root / "BUILD_CPU_PROFILE.json"
        if not metadata.is_file():
            raise ValueError("Native package is missing its build CPU feature metadata")
        data = json.loads(metadata.read_text())
        if (data.get("schema") != SCHEMA or data.get("profile") != "native"
                or data.get("march") != "native" or not data.get("required_features")):
            raise ValueError("Invalid native package CPU metadata")
        # Deliberately conservative: native is intended for its build machine,
        # and can be rejected on another kernel/VM even with a similar CPU.
        required = set(data["required_features"])
    else:
        raise ValueError("Unknown package CPU target: " + march)
    missing = required - host_features(cpuinfo)
    if missing:
        raise ValueError(f"This runtime requires {march}; missing CPU features: " + ", ".join(sorted(missing)))


def check_output(root, metadata):
    root = Path(root)
    marker = root / "BUILD_CPU_TARGET"
    if not marker.is_file():
        return
    new = json.loads(Path(metadata).read_text())
    old_metadata = root / "BUILD_CPU_PROFILE.json"
    if old_metadata.is_file():
        old = json.loads(old_metadata.read_text())
        compatible = old.get("cache_tag") == new["cache_tag"] and old.get("march") == new["march"]
    else:
        compatible = marker.read_text().strip() == new["march"] != "native"
    if not compatible:
        raise ValueError("Output contains another CPU profile/native CPU. Select a fresh XR_OUT_ROOT directory: " + str(root))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    desc = commands.add_parser("describe")
    desc.add_argument("profile")
    desc.add_argument("--output", type=Path)
    check = commands.add_parser("check")
    check.add_argument("root", type=Path)
    output = commands.add_parser("check-output")
    output.add_argument("root", type=Path)
    output.add_argument("metadata", type=Path)
    args = parser.parse_args()
    try:
        if args.command == "describe":
            data = describe(args.profile)
            if args.output:
                args.output.write_text(json.dumps(data, indent=2) + "\n")
            print("\t".join(data[k] for k in ("profile", "march", "tune", "opencv_baseline", "cache_tag")))
        elif args.command == "check":
            check_package(args.root)
        else:
            check_output(args.root, args.metadata)
    except (ValueError, OSError) as exc:
        parser.exit(2, f"[cpu-target][ERROR] {exc}\n")


if __name__ == "__main__":
    main()
