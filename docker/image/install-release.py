#!/usr/bin/env python3
"""Install XR Gate from verified GitHub release assets or local archives."""
import argparse
import hashlib
import json
from pathlib import Path
import platform
import re
import subprocess
import tarfile
import tempfile
from urllib.parse import quote

REPOSITORY = "vladoshub/xr-gate"
FRAME_ARCHIVE = "body-tracking-models-frame.tar.gz"


def release_assets():
    machine = platform.machine().lower()
    arch = {"x86_64": "x64", "amd64": "x64", "aarch64": "arm64", "arm64": "arm64"}.get(machine)
    if arch is None:
        raise RuntimeError(f"Unsupported XR Gate image architecture: {machine}")
    return {
        "runtime": f"xr-gate-ubuntu-24.04-linux-{arch}.tar.gz",
        "models": "hand-tracking-models-mercury.tar.gz",
    }


def download(url, destination):
    subprocess.run(["curl", "--fail", "--location", "--retry", "3", "--connect-timeout", "20",
                    "--max-time", "600", "--user-agent", "xr-gate-docker",
                    "--output", str(destination), url], check=True)


def extract_checked(archive, destination):
    # The data filter checks links against the tree as it is extracted.
    try:
        with tarfile.open(archive, "r:gz") as package:
            package.extractall(destination, filter="data")
    except tarfile.FilterError as error:
        raise RuntimeError(f"Unsafe archive path or member: {error}") from error


def download_release(version, names, archive_dir):
    tag = quote(version, safe="")
    metadata = archive_dir / "release.json"
    download(f"https://api.github.com/repos/{REPOSITORY}/releases/tags/{tag}", metadata)
    release = json.loads(metadata.read_text())
    if release.get("tag_name") != version or release.get("draft"):
        raise RuntimeError(f"Published release not found: {version}")
    assets = {asset["name"]: asset for asset in release.get("assets", [])}
    digests = {}
    for name in names:
        digest = assets.get(name, {}).get("digest") or ""
        if not re.fullmatch(r"sha256:[0-9a-fA-F]{64}", digest):
            raise RuntimeError(f"Release {version} is missing {name} or its SHA-256 digest")
        digests[name] = digest.removeprefix("sha256:").lower()
    for name, expected_digest in digests.items():
        archive = archive_dir / name
        download(f"https://github.com/{REPOSITORY}/releases/download/{tag}/{name}", archive)
        with archive.open("rb") as source:
            digest = hashlib.file_digest(source, "sha256").hexdigest()
        if digest != expected_digest:
            raise RuntimeError(f"SHA256 mismatch: {name}: {digest}")


def install(version, destination=Path("/opt"), *, archives_dir=None):
    if not re.fullmatch(r"[A-Za-z0-9_][A-Za-z0-9_.-]{0,127}", version):
        raise ValueError("XR_VERSION must be a valid image tag, for example v1.2.3")
    selected_assets = release_assets()
    with tempfile.TemporaryDirectory() as temporary:
        archive_dir = Path(temporary) if archives_dir is None else Path(archives_dir)
        if archives_dir is None:
            download_release(version, selected_assets.values(), archive_dir)
        elif (archive_dir / FRAME_ARCHIVE).is_file():
            selected_assets["frame_models"] = FRAME_ARCHIVE

        for name in selected_assets.values():
            if not (archive_dir / name).is_file():
                raise RuntimeError(f"Missing release archive: {archive_dir / name}")

        destination.mkdir(parents=True, exist_ok=True)
        for component, name in selected_assets.items():
            output = destination if component == "runtime" else destination / "xr-gate/bin"
            extract_checked(archive_dir / name, output)
    for name in ("grayscale_detection_160x160.onnx", "grayscale_keypoint_jan18.onnx"):
        if not (destination / "xr-gate/bin/hand-tracking-models/mercury" / name).is_file():
            raise RuntimeError(f"Mercury model missing: {name}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--version", required=True, help="Release and image tag (XR_VERSION)")
    parser.add_argument("--source", choices=("github", "local"), default="github")
    parser.add_argument("--archives-dir", type=Path, help="Directory of local release archives")
    args = parser.parse_args()
    if args.source == "local" and args.archives_dir is None:
        parser.error("--archives-dir is required with --source=local")
    install(args.version, archives_dir=args.archives_dir if args.source == "local" else None)
