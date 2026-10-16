#!/usr/bin/env python3
"""Download pinned release assets, verify hashes, unpack without building anything."""
import hashlib
import json
from pathlib import Path
import subprocess
import tarfile
import tempfile


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


def install(lock_path=Path("/usr/local/share/xr-gate/release.lock.json"), destination=Path("/opt")):
    lock = json.loads(lock_path.read_text())
    destination.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory() as temporary:
        for component in ("runtime", "models"):
            asset = lock[component]
            archive = Path(temporary) / asset["name"]
            url = f'https://github.com/{lock["repository"]}/releases/download/{lock["tag"]}/{asset["name"]}'
            subprocess.run(["curl", "--fail", "--location", "--retry", "3", "--connect-timeout", "20",
                            "--max-time", "600", "--output", str(archive), url], check=True)
            with archive.open("rb") as source:
                digest = hashlib.file_digest(source, "sha256").hexdigest()
            if digest != asset["sha256"]:
                raise RuntimeError(f"SHA256 mismatch: {asset['name']}: {digest}")
            output = destination if component == "runtime" else destination / "xr-gate/bin"
            extract_checked(archive, output)
    for name in ("grayscale_detection_160x160.onnx", "grayscale_keypoint_jan18.onnx"):
        if not (destination / "xr-gate/bin/hand-tracking-models/mercury" / name).is_file():
            raise RuntimeError(f"Mercury model missing: {name}")


if __name__ == "__main__":
    install()
