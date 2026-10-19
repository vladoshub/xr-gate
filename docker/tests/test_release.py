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
    def archives(self):
        archives = {}
        for component, name in installer.ASSETS.items():
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
        archives = self.archives()
        for version in ("v1.2.3", "v1.2.4"):
            with self.subTest(version=version), tempfile.TemporaryDirectory() as folder:
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
                    installer.install(version, Path(folder))
                self.assertEqual(len(urls), 3)
                self.assertTrue(urls[0].endswith(f"/releases/tags/{version}"))
                self.assertEqual((Path(folder) / "xr-gate/bin/release.txt").read_bytes(), b"fixture\n")

    def test_unverified_or_corrupt_assets_are_rejected_before_extraction(self):
        for case in ("missing", "bad-digest", "corrupt", "wrong-tag"):
            with self.subTest(case=case), tempfile.TemporaryDirectory() as folder:
                metadata = {"tag_name": "v1.2.3", "assets": [
                    {"name": name, "digest": "sha256:" + "0" * 64}
                    for name in installer.ASSETS.values()]}
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
