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
        self.env = dict(os.environ, ROOT_PROJECT=str(self.root), XR_BUILD_CPU="avx2")
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
        self.assertIn('"-march=native" ""', port.read_text())
        self.assertIn('add_library(basalt SHARED)', cmake.read_text())
        self.assertIn('backends/basalt_vio', cmake.read_text())
        self.assertNotIn('add_executable(basalt_calibrate', cmake.read_text())
        self.assertEqual(json.loads(manifest.read_text())["dependencies"], ["eigen3", "tbb"])

    def test_all_cpu_profiles_control_flags_opencv_triplets_and_metadata(self):
        code = """
import json, os
from pathlib import Path
names=['XR_BUILD_CPU','XR_CPU_TARGET','XR_CPU_TUNE','XR_OPENCV_CPU_BASELINE','VCPKG_TRIPLET','CFLAGS','CXXFLAGS','XR_CPU_CACHE_TAG']
print(json.dumps({'env':{k:os.environ[k] for k in names},'metadata':json.loads(Path(os.environ['XR_CPU_PROFILE_FILE']).read_text())}))
"""
        for mode, march, tune, opencv in [("AVX2", "x86-64-v3", "generic", "AVX2"),
                                         ("avx512", "x86-64-v4", "generic", "AVX512_SKX"),
                                         ("native", "native", "native", "NATIVE")]:
            with self.subTest(profile=mode):
                r = self.session(code, dict(self.env, XR_BUILD_CPU=mode))
                self.assertEqual(r.returncode, 0, r.stderr)
                data = json.loads(r.stdout)
                env = data["env"]
                self.assertEqual(env["XR_CPU_TARGET"], march)
                self.assertEqual(env["XR_CPU_TUNE"], tune)
                self.assertEqual(env["XR_OPENCV_CPU_BASELINE"], opencv)
                self.assertEqual(env["VCPKG_TRIPLET"], f"x64-linux-{mode.lower()}-release")
                for variable in ("CFLAGS", "CXXFLAGS"):
                    self.assertIn(f"-march={march} -mtune={tune}", env[variable])
                self.assertEqual(data["metadata"]["march"], march)
                self.assertEqual(data["metadata"]["cache_tag"], env["XR_CPU_CACHE_TAG"])
                triplet = ROOT / "backends/basalt_vio/cmake/triplets" / (env["VCPKG_TRIPLET"] + ".cmake")
                self.assertIn(f"-march={march} -mtune={tune}", triplet.read_text())

    def test_invalid_profile_stops_before_build_command(self):
        result = self.session("raise AssertionError('build must not start')", dict(self.env, XR_BUILD_CPU="invalid"))
        self.assertEqual(result.returncode, 2)
        self.assertIn("XR_BUILD_CPU must be", result.stderr)
        self.assertEqual(list((self.root / "build/.sessions").iterdir()), [])

    def test_local_output_directories_are_separate(self):
        for profile, suffix in [("avx2", "out/xr-gate"), ("AvX512", "out/avx512/xr-gate"),
                                ("native", "out/native/xr-gate")]:
            env = dict(self.env, XR_ROOT_PROJECT=str(self.root), XR_OUT_PACKAGE_NAME="xr-gate", XR_BUILD_CPU=profile)
            for variable in ("XR_OUT_ROOT", "XR_OUT_BIN_ROOT"):
                env.pop(variable, None)
            r = subprocess.run(["bash", "-c", 'source "$1"; printf "%s" "$XR_OUT_ROOT"',
                                "test", str(BUILD / "device_out_env.sh")],
                               env=env, capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertEqual(r.stdout, str(self.root / suffix))

    def test_output_guard_rejects_mixing_cpu_profiles(self):
        spec = importlib.util.spec_from_file_location("cpu_target", BUILD.parent / "runtime/cpu_target.py")
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        (self.root / "BUILD_CPU_TARGET").write_text("x86-64-v3\n")
        fresh = self.root / "fresh.json"
        fresh.write_text(json.dumps(module.describe("avx2")))
        module.check_output(self.root, fresh)  # Compatible with the previous AVX2 patch.
        fresh.write_text(json.dumps(module.describe("avx512")))
        with self.assertRaisesRegex(ValueError, "another CPU profile"):
            module.check_output(self.root, fresh)

    def test_runtime_rejects_unsupported_avx512_and_native_cpu(self):
        spec = importlib.util.spec_from_file_location("cpu_target", BUILD.parent / "runtime/cpu_target.py")
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        def cpuinfo(flags, model="test CPU"):
            return "model name : " + model + "\nflags : " + " ".join(sorted(flags)) + "\n"
        v3 = cpuinfo(module.V3_FLAGS)
        v4 = cpuinfo(module.V4_FLAGS)
        marker = self.root / "BUILD_CPU_TARGET"
        marker.write_text("x86-64-v3\n")
        module.check_package(self.root, v3)
        marker.write_text("x86-64-v4\n")
        with self.assertRaisesRegex(ValueError, "missing CPU features"):
            module.check_package(self.root, v3)
        module.check_package(self.root, v4)
        native = module.describe("native", v4)
        self.assertNotEqual(native["cache_tag"], module.describe("native", v3)["cache_tag"])
        self.assertNotEqual(native["cache_tag"], module.describe("native", cpuinfo(module.V4_FLAGS, "another CPU"))["cache_tag"])
        marker.write_text("native\n")
        with self.assertRaisesRegex(ValueError, "missing its build CPU"):
            module.check_package(self.root, v4)
        (self.root / "BUILD_CPU_PROFILE.json").write_text(json.dumps(native))
        module.check_package(self.root, v4)
        with self.assertRaisesRegex(ValueError, "missing CPU features"):
            module.check_package(self.root, v3)

    def test_old_opengv_avx2_override_is_upgraded(self):
        spec = importlib.util.spec_from_file_location("prepare_runtime_build",
                ROOT / "backends/basalt_vio/scripts/prepare_runtime_build.py")
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        upstream = self.root / "basalt"
        upstream.mkdir()
        (upstream / "CMakeLists.txt").write_text('add_library(basalt SHARED)\n# XR_GATE_RUNTIME_ONLY\n'
            '# ---- XR Tracking external backends ----\n')
        (upstream / "vcpkg.json").write_text('{"dependencies": ["eigen3"]}')
        port = upstream / "vcpkg/ports/opengv/portfile.cmake"
        port.parent.mkdir(parents=True)
        port.write_text('''# XR_GATE_AVX2_OPENGV
if(VCPKG_TARGET_IS_LINUX AND VCPKG_TARGET_ARCHITECTURE STREQUAL "x64")
    vcpkg_replace_string("${SOURCE_PATH}/CMakeLists.txt" "-march=native" "-march=x86-64-v3")
endif()

vcpkg_cmake_configure(SOURCE_PATH "${SOURCE_PATH}")
''')
        module.prepare(upstream, True)
        self.assertNotIn("-march=x86-64-v3", port.read_text())
        self.assertEqual(port.read_text().count("# XR_GATE_CPU_PROFILE_OPENGV"), 1)

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
