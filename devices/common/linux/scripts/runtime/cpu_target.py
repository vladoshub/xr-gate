#!/usr/bin/env python3
"""Describe Linux build profiles and check a packaged CPU requirement."""
import argparse
import hashlib
import json
from pathlib import Path
import platform
import re


V1_FLAGS = {"cmov", "cx8", "fpu", "fxsr", "lm", "mmx", "sse", "sse2", "syscall"}
V3_FLAGS = V1_FLAGS | {"avx", "avx2", "fma", "f16c", "bmi1", "bmi2", "lzcnt", "movbe", "xsave",
            "cx16", "lahf_lm", "popcnt", "sse3", "ssse3", "sse4_1", "sse4_2"}
V4_FLAGS = V3_FLAGS | {"avx512f", "avx512bw", "avx512cd", "avx512dq", "avx512vl"}
AARCH64_FLAGS = {"fp", "asimd"}
RV64GC_FLAGS = set("imafdc")
# GCC 15 in Ubuntu 26.04 needs the RVA23U64 extensions spelled out.
RVA23_MARCH = (
    "rv64gcbv_zic64b_zicbom_zicbop_zicboz_ziccamoa_ziccif_zicclsm_ziccrse_zicntr_"
    "zicond_zihintntl_zihintpause_zihpm_zimop_za64rs_zawrs_zfa_zfhmin_zcb_zcmop_"
    "zkt_zvbb_zvfhmin_zvkt_supm"
)
PROFILES = {
    "generic": ("x86_64", "x86-64", "generic", "SSE2"),
    "avx2": ("x86_64", "x86-64-v3", "generic", "AVX2"),
    "avx512": ("x86_64", "x86-64-v4", "generic", "AVX512_SKX"),
    "native": ("x86_64", "native", "native", "NATIVE"),
    "aarch64": ("aarch64", "armv8-a", "generic", "NEON"),
    "riscv64": ("riscv64", "rv64gc", "rocket", "DETECT"),
    "rva23s64": ("riscv64", RVA23_MARCH, "rocket", "DETECT"),
}
TARGET_ARCHITECTURES = {march: architecture for architecture, march, _, _ in PROFILES.values()}
PORTABLE_FEATURES = {
    "x86-64": V1_FLAGS,
    "x86-64-v3": V3_FLAGS,
    "x86-64-v4": V4_FLAGS,
    "armv8-a": AARCH64_FLAGS,
    "rv64gc": RV64GC_FLAGS,
    # Check the vector and bit-manipulation extensions reported by Linux.
    RVA23_MARCH: RV64GC_FLAGS | {"v", "zba", "zbb", "zbs", "zicond", "zvbb", "zvfhmin"},
}
SCHEMA = "xr_gate_cpu_profile_v1"


def normalize_flags(flags):
    result = set(flags)
    if "abm" in result:
        result.add("lzcnt")
    if "pni" in result:
        result.add("sse3")
    return result


def check_architecture(architecture, operation):
    machine = platform.machine().lower()
    detected = {"amd64": "x86_64", "arm64": "aarch64"}.get(machine, machine)
    if detected != architecture:
        raise ValueError(f"{operation} requires Linux {architecture}; detected {machine}")


def host_features(cpuinfo=None, architecture="x86_64"):
    text = Path("/proc/cpuinfo").read_text() if cpuinfo is None else cpuinfo
    if architecture == "riscv64":
        # Linux reports a common ISA and may also list the ISA of each hart.
        isas = re.findall(r"^(?:hart )?isa[ \t]*:[ \t]*rv64([a-z]+)((?:_[a-z0-9]+)*)", text, re.M)
        if not isas:
            raise ValueError("Cannot determine riscv64 CPU features from /proc/cpuinfo")
        harts = []
        for letters, extensions in isas:
            flags = set(letters.replace("g", "imafd")).union(extensions.split("_")[1:])
            if "b" in flags:
                flags.update({"zba", "zbb", "zbs"})
            harts.append(flags)
        return set.intersection(*harts)

    label = "Features" if architecture == "aarch64" else "flags"
    cpus = re.findall(rf"^{label}\s*:\s*(.*)$", text, re.M)
    if not cpus:
        raise ValueError(f"Cannot determine {architecture} CPU features from /proc/cpuinfo")
    # Every schedulable CPU must support the requirement.
    return set.intersection(*(normalize_flags(cpu.split()) for cpu in cpus))


def describe(mode, cpuinfo=None, check_host=False):
    mode = mode.strip().lower()
    if mode not in PROFILES:
        raise ValueError(f"XR_BUILD_CPU must be one of {', '.join(PROFILES)}")
    architecture, march, tune, baseline = PROFILES[mode]
    if check_host:
        check_architecture(architecture, f"XR_BUILD_CPU={mode}")
    if mode == "native":
        text = Path("/proc/cpuinfo").read_text() if cpuinfo is None else cpuinfo
        features = host_features(text)
        identity = re.findall(r"^(?:vendor_id|cpu family|model|stepping|model name)\s*:\s*(.*)$", text, re.M)
        identity = "\n".join(sorted(set(identity)) + sorted(features))
        cache_tag = "native-" + hashlib.sha256(identity.encode()).hexdigest()[:24]
    else:
        features = PORTABLE_FEATURES[march]
        cache_tag = mode if mode == "rva23s64" else march
    return dict(schema=SCHEMA, profile=mode, architecture=architecture, march=march, tune=tune,
                opencv_baseline=baseline, cache_tag=cache_tag,
                required_features=sorted(features))


def check_package(root, cpuinfo=None):
    root = Path(root)
    marker = root / "BUILD_CPU_TARGET"
    if not marker.is_file():
        return
    march = marker.read_text().strip()
    if march in PORTABLE_FEATURES:
        required = PORTABLE_FEATURES[march]
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
    architecture = TARGET_ARCHITECTURES[march]
    check_architecture(architecture, "This runtime")
    missing = required - host_features(cpuinfo, architecture)
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
        compatible = (old.get("cache_tag") == new["cache_tag"] and old.get("march") == new["march"]
                      and old.get("architecture", new["architecture"]) == new["architecture"])
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
    desc.add_argument("--check-host", action="store_true", help="Require the build host architecture")
    check = commands.add_parser("check")
    check.add_argument("root", type=Path)
    output = commands.add_parser("check-output")
    output.add_argument("root", type=Path)
    output.add_argument("metadata", type=Path)
    args = parser.parse_args()
    try:
        if args.command == "describe":
            data = describe(args.profile, check_host=args.check_host)
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
