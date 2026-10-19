#!/usr/bin/env python3
"""Assemble or validate the Windows installer and selected SteamVR variants."""
import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import re
import shutil
import tempfile
from zipfile import ZipFile, ZIP_DEFLATED

ROOT_NAME = "xr-gate-windows"
REQUIRED = ("INSTALL.cmd", "RUN.cmd", "DISPLAY.cmd", "USB.cmd", "XrGate-Wsl.ps1",
            ".env.example", "windows/Environment.ps1",
            "windows/Display.ps1", "windows/DisplayNative.cs", "linux/xr_gate_wsl.py",
            "README.md", "README.ru.md", "START-HERE.html", "START-HERE.ru.html")


def selections(frequencies, modes):
    frequencies = list(dict.fromkeys(filter(None, re.split(r"[\s,;]+", frequencies))))
    modes = list(dict.fromkeys(filter(None, re.split(r"[\s,;]+", modes))))
    if not frequencies or not modes:
        raise ValueError("Select at least one frequency and display mode.")
    if any(not re.fullmatch(r"[1-9][0-9]{0,3}", value) or int(value) > 1000 for value in frequencies):
        raise ValueError("Frequencies must be integer Hz values from 1 to 1000.")
    if any(value not in ("direct", "extended_sbs") for value in modes):
        raise ValueError("Modes must be direct or extended_sbs.")
    return [(int(hz), mode) for hz in frequencies for mode in modes]


def variant_path(hz, mode):
    return f"drivers/{hz}Hz-{mode}/xr_tracking"


def hardware_preset(env):
    match = re.search(r"(?m)^XR_HARDWARE_ENV=(.+)$", env)
    relative = PurePosixPath(match.group(1).strip().strip("\"'")) if match else None
    if not relative or relative.is_absolute() or ".." in relative.parts or "\\" in str(relative):
        raise ValueError("XR_HARDWARE_ENV must select a preset inside the Windows bundle.")
    return relative.as_posix()


def verify(path, frequencies, modes):
    expected = selections(frequencies, modes)
    with ZipFile(path) as package:
        names = package.namelist()
        if len(names) != len(set(names)):
            raise ValueError("Duplicate ZIP entries.")
        for name in names:
            parts = PurePosixPath(name).parts
            if not parts or parts[0] != ROOT_NAME or ".." in parts or "\\" in name:
                raise ValueError("Unexpected ZIP entry: " + name)
        for name in (*REQUIRED, "bundle.json"):
            if f"{ROOT_NAME}/{name}" not in names:
                raise ValueError("Bundle file missing: " + name)
        env = package.read(f"{ROOT_NAME}/.env.example").decode("utf-8-sig")
        preset = hardware_preset(env)
        if f"{ROOT_NAME}/{preset}" not in names:
            raise ValueError("Hardware preset missing from bundle: " + preset)
        manifest = json.loads(package.read(f"{ROOT_NAME}/bundle.json"))
        actual = {(entry["frequency_hz"], entry["display_mode"]) for entry in manifest["variants"]}
        if manifest.get("schema") != 1 or actual != set(expected):
            raise ValueError("Bundle variants do not match the requested CI selection.")
        for hz, mode in expected:
            prefix = f"{ROOT_NAME}/{variant_path(hz, mode)}"
            for name in ("driver.vrdrivermanifest", "bin/win64/driver_xr_tracking.dll", "bin/win64/openvr_api.dll",
                         "resources/settings/default.vrsettings", "LICENSE", "THIRD_PARTY_NOTICES.md"):
                if f"{prefix}/{name}" not in names:
                    raise ValueError("Driver file missing: " + prefix + "/" + name)
            settings = json.loads(package.read(prefix + "/resources/settings/default.vrsettings"))["xr_tracking"]
            if settings.get("displayFrequency") != hz:
                raise ValueError("Unexpected driver frequency: " + prefix)
            for key in ("runtimePoseTransport", "runtimeHandTransport", "runtimeControllerStateTransport"):
                if settings.get(key) != "tcp":
                    raise ValueError("Windows driver must use TCP: " + prefix)
            if settings.get("isDisplayOnDesktop") != (mode == "extended_sbs"):
                raise ValueError("Unexpected display mode: " + prefix)
    return manifest


def build(source, drivers, output, frequencies, modes, release_tag="latest"):
    variants = selections(frequencies, modes)
    if not re.fullmatch(r"[A-Za-z0-9_.-]+", release_tag):
        raise ValueError("Invalid release tag.")
    with tempfile.TemporaryDirectory() as temporary:
        bundle = Path(temporary) / ROOT_NAME
        bundle.mkdir()
        for path in sorted(source.rglob("*")):
            relative = path.relative_to(source)
            if not path.is_file() or any(part.startswith(".") or part in ("tests", "__pycache__") for part in relative.parts):
                continue
            if relative.name == "package_windows.py":
                continue
            target = bundle / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(path, target)
        shutil.copy2(source / ".env.example", bundle / ".env.example")
        env = (bundle / ".env.example").read_text(encoding="utf-8")
        env = re.sub(r"(?m)^XR_RELEASE_TAG=.*$", "XR_RELEASE_TAG=" + release_tag, env)
        (bundle / ".env.example").write_text(env, encoding="utf-8", newline="\n")
        # Make the first install select a variant included in this CI run.
        preset = bundle / hardware_preset(env)
        content = preset.read_text(encoding="utf-8")
        values = dict(re.findall(r"(?m)^(XR_[A-Z_]+)=(.*)$", content))
        hz = values.get("XR_DISPLAY_FREQUENCY_HZ", "")
        preferred = (int(hz), values.get("XR_DISPLAY_MODE", "")) if hz.isdigit() else None
        if preferred not in variants:
            preferred = variants[0]
        for key, value in (("XR_DISPLAY_FREQUENCY_HZ", str(preferred[0])), ("XR_DISPLAY_MODE", preferred[1])):
            content = re.sub(r"(?m)^" + key + "=.*$", key + "=" + value, content)
        preset.write_text(content, encoding="utf-8", newline="\n")
        entries = []
        for hz, mode in variants:
            relative = variant_path(hz, mode)
            package = drivers / f"{hz}Hz-{mode}" / "xr_tracking"
            if not package.is_dir():
                raise FileNotFoundError("Selected Windows driver was not built: " + str(package))
            target = bundle / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copytree(package, target)
            entries.append({"frequency_hz": hz, "display_mode": mode, "path": relative})
        (bundle / "bundle.json").write_text(json.dumps({
            "schema": 1, "release_tag": release_tag, "variants": entries,
        }, indent=2) + "\n", encoding="utf-8", newline="\n")
        output.parent.mkdir(parents=True, exist_ok=True)
        with ZipFile(output, "w", ZIP_DEFLATED) as package:
            for path in sorted(bundle.rglob("*")):
                if path.is_file():
                    package.write(path, path.relative_to(bundle.parent).as_posix())
    verify(output, frequencies, modes)
    with output.open("rb") as stream:
        digest = hashlib.file_digest(stream, "sha256").hexdigest()
    output.with_name(output.name + ".sha256").write_text(digest + "  " + output.name + "\n", encoding="ascii", newline="\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    create = commands.add_parser("build")
    create.add_argument("--source", type=Path, default=Path(__file__).resolve().parent)
    create.add_argument("--drivers-root", type=Path, required=True)
    create.add_argument("--output", type=Path, required=True)
    create.add_argument("--release-tag", default="latest")
    check = commands.add_parser("verify")
    check.add_argument("archive", type=Path)
    for command in (create, check):
        command.add_argument("--frequencies", required=True)
        command.add_argument("--modes", required=True)
    args = parser.parse_args()
    if args.command == "build":
        build(args.source, args.drivers_root, args.output, args.frequencies, args.modes, args.release_tag)
    else:
        verify(args.archive, args.frequencies, args.modes)
    print("OK Windows bundle: " + str(args.output if args.command == "build" else args.archive))


if __name__ == "__main__":
    main()
