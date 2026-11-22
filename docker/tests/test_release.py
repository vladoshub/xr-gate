import hashlib
import importlib.util
import io
import json
from pathlib import Path
import tarfile
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location(
    "release_installer", Path(__file__).resolve().parents[1] / "image/install-release.py")
installer = importlib.util.module_from_spec(spec)
spec.loader.exec_module(installer)


class ReleaseTest(unittest.TestCase):
    def archives(self, ubuntu_version="24.04"):
        archives = {}
        for component, name in installer.release_assets(ubuntu_version).items():
            content = io.BytesIO()
            with tarfile.open(fileobj=content, mode="w:gz") as package:
                paths = (["xr-gate/bin/release.txt"] if component == "runtime" else [
                    "hand-tracking-models/mercury/grayscale_detection_160x160.onnx",
                    "hand-tracking-models/mercury/grayscale_keypoint_jan18.onnx"])
                for path in paths:
                    member = tarfile.TarInfo(path)
                    data = b"fixture\n"
                    member.size = len(data)
                    package.addfile(member, io.BytesIO(data))
            archives[name] = content.getvalue()
        return archives

    def test_selected_release_downloads_and_verifies_both_assets(self):
        builds = (
            ("v1.2.3", "x86_64", "x64", "24.04"),
            ("v1.2.4", "aarch64", "arm64", "24.04"),
            ("v1.2.5", "riscv64", "riscv64", "24.04"),
            ("v1.2.5", "riscv64", "riscv64", "26.04"),
        )
        for version, machine, arch, ubuntu in builds:
            with self.subTest(version=version, machine=machine, ubuntu=ubuntu), \
                 patch.object(installer.platform, "machine", return_value=machine), \
                 tempfile.TemporaryDirectory() as folder:
                archives = self.archives(ubuntu)
                metadata = {"tag_name": version, "assets": [
                    {"name": name, "digest": "sha256:" + hashlib.sha256(data).hexdigest()}
                    for name, data in archives.items()]}
                urls = []

                def download(url, destination):
                    urls.append(url)
                    if "/releases/tags/" in url:
                        destination.write_text(json.dumps(metadata))
                    else:
                        self.assertIn(f"/releases/download/{version}/", url)
                        destination.write_bytes(archives[destination.name])

                with patch.object(installer, "download", side_effect=download):
                    installer.install(version, Path(folder), ubuntu_version=ubuntu)
                self.assertEqual(len(urls), 3)
                self.assertTrue(urls[0].endswith(f"/releases/tags/{version}"))
                self.assertTrue(urls[1].endswith(f"/xr-gate-ubuntu-{ubuntu}-linux-{arch}.tar.gz"))
                self.assertEqual((Path(folder) / "xr-gate/bin/release.txt").read_bytes(), b"fixture\n")

    def test_install_from_local_archives_with_optional_frame(self):
        builds = (
            ("x86_64", "24.04", True),
            ("aarch64", "24.04", False),
            ("riscv64", "24.04", True),
            ("riscv64", "26.04", False),
        )
        for machine, ubuntu, include_frame in builds:
            with self.subTest(machine=machine, ubuntu=ubuntu), \
                 patch.object(installer.platform, "machine", return_value=machine), \
                 tempfile.TemporaryDirectory() as folder:
                root = Path(folder)
                archives_dir = root / "archives"
                archives_dir.mkdir()
                for name, data in self.archives(ubuntu).items():
                    (archives_dir / name).write_bytes(data)
                frame_path = "body-tracking-models/frame/frame_backbone.onnx"
                if include_frame:
                    with tarfile.open(archives_dir / installer.FRAME_ARCHIVE, "w:gz") as package:
                        model = tarfile.TarInfo(frame_path)
                        model.size = len(b"frame model")
                        package.addfile(model, io.BytesIO(b"frame model"))

                with patch.object(installer, "download") as download:
                    installer.install("v1.2.3", root / "out", ubuntu_version=ubuntu,
                                      archives_dir=archives_dir)
                download.assert_not_called()
                bin_dir = root / "out/xr-gate/bin"
                self.assertEqual((bin_dir / "release.txt").read_bytes(), b"fixture\n")
                self.assertEqual((bin_dir / frame_path).is_file(), include_frame)

    def test_missing_local_archive_stops_before_extraction(self):
        with tempfile.TemporaryDirectory() as folder, \
             patch.object(installer, "download") as download, \
             patch.object(installer, "extract_checked") as extract:
            root = Path(folder)
            runtime = installer.release_assets()["runtime"]
            (root / runtime).write_bytes(self.archives()[runtime])
            with self.assertRaisesRegex(RuntimeError, "Missing release archive: .*hand-tracking-models-mercury"):
                installer.install("v1.2.3", root / "out", archives_dir=root)
            download.assert_not_called()
            extract.assert_not_called()

    def test_unverified_or_corrupt_assets_are_rejected_before_extraction(self):
        for case in ("missing", "bad-digest", "corrupt", "wrong-tag"):
            with self.subTest(case=case), tempfile.TemporaryDirectory() as folder:
                metadata = {"tag_name": "v1.2.3", "assets": [
                    {"name": name, "digest": "sha256:" + "0" * 64}
                    for name in installer.release_assets().values()]}
                if case == "missing":
                    metadata["assets"].pop()
                if case == "bad-digest":
                    metadata["assets"][0]["digest"] = None
                if case == "wrong-tag":
                    metadata["tag_name"] = "v9.9.9"

                def download(url, destination):
                    if "/releases/tags/" in url:
                        destination.write_text(json.dumps(metadata))
                    else:
                        destination.write_bytes(b"corrupt archive")

                with patch.object(installer, "download", side_effect=download), \
                     patch.object(installer, "extract_checked") as extract:
                    with self.assertRaises(RuntimeError):
                        installer.install("v1.2.3", Path(folder))
                    extract.assert_not_called()


if __name__ == "__main__":
    unittest.main()
