#!/usr/bin/env python3
"""Build orchestration checks; no compiler, downloads, Docker or headset needed."""
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

BUILD = Path(__file__).resolve().parents[1]
ROOT = BUILD.parents[4]


class NativeBuildTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.env = dict(os.environ, ROOT_PROJECT=str(self.root))
        for name in ("XR_BUILD_SESSION_DIR", "XR_BUILD_SESSION_OWNER", "XR_LOCAL_BUILD_ID",
                     "GITHUB_RUN_ID", "GITHUB_RUN_ATTEMPT"):
            self.env.pop(name, None)

    def session(self, script, env=None):
        return subprocess.run(["bash", str(BUILD / "with_build_session.sh"), sys.executable,
                               "-c", script], env=env or self.env, capture_output=True, text=True)

    def test_new_session_never_reuses_cache_and_cleans_on_failure(self):
        code = """
import json, os
from pathlib import Path
p=Path(os.environ['CCACHE_DIR'])/'old-result'
assert not p.exists()
p.touch()
print(json.dumps({k:os.environ[k] for k in ['XR_BUILD_SESSION_DIR','VCPKG_BINARY_SOURCES','CCACHE_DIR']}))
"""
        first = self.session(code)
        second = self.session(code)
        self.assertEqual(first.returncode, 0, first.stderr)
        self.assertEqual(second.returncode, 0, second.stderr)
        one, two = json.loads(first.stdout), json.loads(second.stdout)
        self.assertNotEqual(one["XR_BUILD_SESSION_DIR"], two["XR_BUILD_SESSION_DIR"])
        self.assertTrue(one["VCPKG_BINARY_SOURCES"].startswith("clear;files,"))
        self.assertFalse(Path(one["XR_BUILD_SESSION_DIR"]).exists())
        failed = self.session("import os; print(os.environ['XR_BUILD_SESSION_DIR']); raise SystemExit(7)")
        self.assertEqual(failed.returncode, 7)
        self.assertFalse(Path(failed.stdout.strip()).exists())

    def test_sdk_reuse_requires_same_run_attempt_distro_and_cpu(self):
        code = r"""
import os, subprocess
from pathlib import Path
prefix=Path(os.environ['XR_BUILD_SESSION_DIR'])/'sdk'
(prefix/'lib/cmake/opencv4').mkdir(parents=True)
(prefix/'lib/cmake/opencv4/OpenCVConfig.cmake').touch()
(prefix/'licenses').mkdir()
(prefix/'licenses/OpenCV-LICENSE.txt').touch()
release=dict(line.rstrip().split('=',1) for line in Path('/etc/os-release').read_text().splitlines() if '=' in line)
release={k:v.strip('"') for k,v in release.items()}
token=os.environ['XR_BUILD_DEPENDENCY_TOKEN']
expected=f"{token}|{release['ID']}:{release['VERSION_ID']}|71d3237a093b60a27601c20e9ee6c3e52154e8b1|x86-64-v3"
(prefix/'XR_BUILD_ID').write_text(expected+'\n')
env=dict(os.environ,XR_OPENCV_PREFIX=str(prefix))
script=os.environ['TEST_OPENCV_SCRIPT']
for _ in range(2):
    r=subprocess.run(['bash',script],env=env,capture_output=True,text=True)
    assert r.returncode==0,r.stderr
for replacement in ['ci:123:2',expected.replace('x86-64-v3','native'),expected.replace(release['VERSION_ID'],'0.0')]:
    (prefix/'XR_BUILD_ID').write_text(replacement+'\n')
    r=subprocess.run(['bash',script],env=env,capture_output=True,text=True)
    assert r.returncode==2,r.stderr
"""
        env = dict(self.env, GITHUB_RUN_ID="123", GITHUB_RUN_ATTEMPT="1",
                   TEST_OPENCV_SCRIPT=str(BUILD / "build_opencv.sh"))
        result = self.session(code, env)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_runtime_preparation_is_idempotent_and_keeps_core(self):
        spec = importlib.util.spec_from_file_location("prepare_runtime_build",
                ROOT / "backends/basalt_vio/scripts/prepare_runtime_build.py")
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        upstream = self.root / "basalt"
        upstream.mkdir()
        port = upstream / "vcpkg/ports/opengv/portfile.cmake"
        port.parent.mkdir(parents=True)
        port.write_text('vcpkg_cmake_configure(SOURCE_PATH "${SOURCE_PATH}")\n')
        cmake = upstream / "CMakeLists.txt"
        cmake.write_text('find_package(Pangolin CONFIG REQUIRED)\nfind_package(GTest CONFIG REQUIRED)\n'
                         'add_library(basalt SHARED)\n'
                         'set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "-O3 -g -DEIGEN_INITIALIZE_MATRICES_BY_NAN")\n'
                         'add_executable(basalt_calibrate src/calibrate.cpp)\n'
                         'add_subdirectory(test)\n# ---- XR Tracking external backends ----\n'
                         'add_subdirectory("${XR_TRACKING_ROOT}/backends/basalt_vio")\n')
        manifest = upstream / "vcpkg.json"
        manifest.write_text(json.dumps({"dependencies": ["eigen3", "gtest", "pangolin", "tbb",
                                      {"name": "opencv4"}, {"name": "realsense2"}]}))
        module.prepare(upstream, True)
        once = cmake.read_bytes(), manifest.read_bytes(), port.read_bytes()
        module.prepare(upstream, True)
        self.assertEqual(once, (cmake.read_bytes(), manifest.read_bytes(), port.read_bytes()))
        self.assertIn('"-march=native" "-march=x86-64-v3"', port.read_text())
        self.assertIn('add_library(basalt SHARED)', cmake.read_text())
        self.assertIn('backends/basalt_vio', cmake.read_text())
        self.assertNotIn('add_executable(basalt_calibrate', cmake.read_text())
        self.assertEqual(json.loads(manifest.read_text())["dependencies"], ["eigen3", "tbb"])

    @unittest.skipUnless(shutil.which("readelf") and shutil.which("strip"), "binutils needed")
    def test_package_strip_does_not_follow_external_symlinks(self):
        spec = importlib.util.spec_from_file_location("finalize_native_package", BUILD / "finalize_native_package.py")
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        binary = Path(shutil.which("true"))
        bin_dir = self.root / "bin"
        bin_dir.mkdir()
        shutil.copy2(binary, bin_dir / "tool")
        (bin_dir / "external-python").symlink_to(binary)
        before = binary.stat().st_mtime_ns
        self.assertEqual(module.finalize(self.root, True), 1)
        self.assertEqual(binary.stat().st_mtime_ns, before)
        subprocess.run([str(bin_dir / "tool")], check=True)


if __name__ == "__main__":
    unittest.main()
