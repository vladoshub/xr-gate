#!/usr/bin/env python3
"""Download the selected release, verify GitHub SHA-256 digests, and unpack it."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import tarfile
import tempfile
from urllib.parse import quote

REPOSITORY = "vladoshub/xr-gate"
ASSETS = {
    "runtime": "xr-gate-ubuntu-24.04-linux-x64.tar.gz",
    "models": "hand-tracking-models-mercury.tar.gz",
}


def download(url, destination):
    subprocess.run(["curl", "--fail", "--location", "--retry", "3", "--connect-timeout", "20",
                    "--max-time", "600", "--user-agent", "xr-gate-docker",
                    "--output", str(destination), url], check=True)


def extract_checked(archive, destination):
    # The release contains relative library symlinks. Reject escaping paths/links.
    root = Path(destination).resolve()
    with tarfile.open(archive, "r:gz") as package:
        for member in package.getmembers():
            target = (root / member.name).resolve()
            if target != root and root not in target.parents:
                raise RuntimeError(f"Unsafe archive path: {member.name}")
            if member.isdev() or member.isfifo():
                raise RuntimeError(f"Unsupported archive member: {member.name}")
            if member.issym() or member.islnk():
                base = target.parent if member.issym() else root
                link = (base / member.linkname).resolve()
                if link != root and root not in link.parents:
                    raise RuntimeError(f"Unsafe archive link: {member.name}")
    subprocess.run(["tar", "--no-same-owner", "-xzf", str(archive), "-C", str(root)], check=True)


def install(version, destination=Path("/opt")):
    if not re.fullmatch(r"[A-Za-z0-9_][A-Za-z0-9_.-]{0,127}", version):
        raise ValueError("XR_VERSION must be a release tag, for example v1.2.3")
    destination.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory() as temporary:
        metadata = Path(temporary) / "release.json"
        download(f"https://api.github.com/repos/{REPOSITORY}/releases/tags/{quote(version, safe='')}", metadata)
        release = json.loads(metadata.read_text())
        if release.get("tag_name") != version or release.get("draft"):
            raise RuntimeError(f"Published release not found: {version}")
        assets = {asset["name"]: asset for asset in release.get("assets", [])}
        digests = {}
        for name in ASSETS.values():
            digest = assets.get(name, {}).get("digest") or ""
            if not re.fullmatch(r"sha256:[0-9a-fA-F]{64}", digest):
                raise RuntimeError(f"Release {version} is missing {name} or its SHA-256 digest")
            digests[name] = digest.removeprefix("sha256:").lower()
        for component, name in ASSETS.items():
            archive = Path(temporary) / name
            download(f"https://github.com/{REPOSITORY}/releases/download/{quote(version, safe='')}/{name}", archive)
            with archive.open("rb") as source:
                digest = hashlib.file_digest(source, "sha256").hexdigest()
            if digest != digests[name]:
                raise RuntimeError(f"SHA256 mismatch: {name}: {digest}")
            output = destination if component == "runtime" else destination / "xr-gate/bin"
            extract_checked(archive, output)
    for name in ("grayscale_detection_160x160.onnx", "grayscale_keypoint_jan18.onnx"):
        if not (destination / "xr-gate/bin/hand-tracking-models/mercury" / name).is_file():
            raise RuntimeError(f"Mercury model missing: {name}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--version", required=True, help="GitHub release tag (XR_VERSION)")
    install(parser.parse_args().version)
