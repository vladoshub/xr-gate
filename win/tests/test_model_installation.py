import contextlib
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import subprocess
import tarfile
import tempfile
import unittest
from unittest.mock import patch


WIN = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("model_install_helper", WIN / "linux/xr_gate_wsl.py")
linux = importlib.util.module_from_spec(spec)
spec.loader.exec_module(linux)


class ModelInstallationTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.runtime = linux.Runtime.__new__(linux.Runtime)
        self.runtime.base = self.root / "installed"
        self.runtime.current = self.runtime.base / "current"
        self.runtime.config = self.runtime.base / "config"
        self.runtime.lock = self.runtime.base / "runtime.lock"
        self.runtime.settings = {}
        self.runtime.user = "testuser"
        self.runtime.root_required = lambda: None
        self.runtime.own = lambda path: None
        self.runtime.as_user = lambda args: subprocess.run([str(arg) for arg in args], check=True)
        self.runtime.cfg = {"tag": "test-release"}
        mocks = self.enterContext(contextlib.ExitStack())
        mocks.enter_context(patch.object(linux, "run"))
        mocks.enter_context(patch.object(linux.Runtime, "ensure_systemd", return_value=True))
        mocks.enter_context(patch.object(linux.platform, "machine", return_value="x86_64"))
        mocks.enter_context(patch.object(linux.platform, "freedesktop_os_release",
                                        return_value={"ID": "ubuntu", "VERSION_ID": "24.04"}))
        self.set_runtime_archive()
        self.set_archive("models", {"hand-tracking-models/mercury/grayscale_detection_160x160.onnx": b"mercury"})
        self.set_body_archive(b"frame-v1")

    def set_archive(self, name, members):
        path = self.root / (name + ".tar.gz")
        with tarfile.open(path, "w:gz") as archive:
            for relative, data in members.items():
                member = tarfile.TarInfo(relative)
                member.size = len(data)
                archive.addfile(member, io.BytesIO(data))
        self.runtime.cfg[name + "_archive"] = str(path)
        self.runtime.cfg[name + "_sha256"] = hashlib.sha256(path.read_bytes()).hexdigest()

    def set_runtime_archive(self, with_frame=True):
        members = {"xr-gate/bin/.fixture": b"",
                   "xr-gate/run_xr_client.sh": b"#!/bin/sh\n",
                   "xr-gate/devices/common/linux/scripts/runtime/install_runtime_deps_ubuntu.sh": b"#!/bin/sh\n"}
        if with_frame:
            members["xr-gate/bin/backends/frame_body_tracking/frame_body_tracking_backend"] = b"frame"
        self.set_archive("runtime", members)

    def set_body_archive(self, data):
        self.set_archive("body_models", {"body-tracking-models/frame/frame_backbone.onnx": data})

    def test_installs_frame_and_mercury_together_and_records_hashes(self):
        self.assertEqual(self.runtime.install(), 0)
        package = self.runtime.current.resolve()
        self.assertEqual((package / "bin/body-tracking-models/frame/frame_backbone.onnx").read_bytes(), b"frame-v1")
        self.assertEqual((package / "bin/hand-tracking-models/mercury/grayscale_detection_160x160.onnx").read_bytes(), b"mercury")
        installed = json.loads((package.parent / "installed.json").read_text())
        self.assertEqual(installed["body_models_sha256"], self.runtime.cfg["body_models_sha256"])

    def test_update_replaces_pre_frame_installer_cache_for_the_same_release(self):
        cfg = self.runtime.cfg
        legacy = self.runtime.base / "releases" / (cfg["tag"] + "-" + cfg["runtime_sha256"][:12] + "-" + cfg["models_sha256"][:8])
        old_package = legacy / "xr-gate"
        old_package.mkdir(parents=True)
        self.runtime.current.symlink_to(old_package)
        self.assertEqual(self.runtime.install(), 0)
        self.assertNotEqual(self.runtime.current.resolve(), old_package)
        self.assertTrue((self.runtime.current / "bin/body-tracking-models/frame/frame_backbone.onnx").is_file())
        self.assertTrue(old_package.is_dir())

    def test_changed_frame_model_creates_a_new_installation(self):
        self.runtime.install()
        first = self.runtime.current.resolve()
        self.set_body_archive(b"frame-v2")
        self.runtime.install()
        self.assertNotEqual(self.runtime.current.resolve(), first)
        self.assertEqual((first / "bin/body-tracking-models/frame/frame_backbone.onnx").read_bytes(), b"frame-v1")
        self.assertEqual((self.runtime.current / "bin/body-tracking-models/frame/frame_backbone.onnx").read_bytes(), b"frame-v2")

    def test_bad_model_hash_keeps_current_installation(self):
        self.runtime.install()
        current = self.runtime.current.resolve()
        self.runtime.cfg["body_models_sha256"] = "0" * 64
        with self.assertRaisesRegex(ValueError, "SHA-256 mismatch"):
            self.runtime.install()
        self.assertEqual(self.runtime.current.resolve(), current)

    def test_missing_or_empty_frame_model_cleans_staging_and_keeps_current(self):
        self.runtime.install()
        current = self.runtime.current.resolve()
        for members in ({"body-tracking-models/frame/SOURCE.txt": b"missing model"},
                        {"body-tracking-models/frame/frame_backbone.onnx": b""}):
            with self.subTest(members=members):
                self.set_archive("body_models", members)
                with self.assertRaisesRegex(RuntimeError, "nonempty"):
                    self.runtime.install()
                self.assertEqual(self.runtime.current.resolve(), current)
                self.assertEqual(list((self.runtime.base / "releases").glob(".install-*")), [])

    def test_frame_requires_its_model_and_older_runtime_still_installs(self):
        del self.runtime.cfg["body_models_archive"]
        del self.runtime.cfg["body_models_sha256"]
        with self.assertRaisesRegex(RuntimeError, "body-tracking-models-frame.tar.gz"):
            self.runtime.install()
        self.assertFalse(self.runtime.current.exists())
        self.assertEqual(list((self.runtime.base / "releases").glob(".install-*")), [])
        self.set_runtime_archive(with_frame=False)
        self.assertEqual(self.runtime.install(), 0)
        self.assertTrue((self.runtime.current / "run_xr_client.sh").is_file())


if __name__ == "__main__":
    unittest.main()
