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
                     "GITHUB_RUN_ID", "GITHUB_RUN_ATTEMPT", "XR_KEEP_OPENCV_BUILD",
                     "XR_OPENCV_PREFIX"):
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

    def test_opencv_is_kept_by_default_on_success_and_failure_without_reusing_caches(self):
        code = """
import json, os
from pathlib import Path
session = Path(os.environ['XR_BUILD_SESSION_DIR'])
assert not (session/'opencv').exists()
assert not (Path(os.environ['CCACHE_DIR'])/'old-result').exists()
for name in ('opencv/build/object.o', 'opencv/install/lib/libopencv_core.a', 'build/other.o'):
    path = session/name
    path.parent.mkdir(parents=True, exist_ok=True)
    path.touch()
(Path(os.environ['CCACHE_DIR'])/'old-result').touch()
print(json.dumps(str(session)))
raise SystemExit(int(os.environ['TEST_EXIT_STATUS']))
"""
        sessions = []
        for status in (0, 7):
            with self.subTest(status=status):
                result = self.session(code, dict(self.env, TEST_EXIT_STATUS=str(status)))
                self.assertEqual(result.returncode, status, result.stderr)
                session = Path(json.loads(result.stdout))
                sessions.append(session)
                self.assertEqual({path.name for path in session.iterdir()}, {"opencv"})
                self.assertTrue((session / "opencv/build/object.o").exists())
                self.assertTrue((session / "opencv/install/lib/libopencv_core.a").exists())
                self.assertIn(str(session / "opencv"), result.stderr)
        self.assertNotEqual(*sessions)

    def test_opencv_retention_can_be_disabled_without_removing_source_checkout(self):
        code = """
import os
from pathlib import Path
session = Path(os.environ['XR_BUILD_SESSION_DIR'])
for name in ('opencv/build/object.o', 'opencv/install/lib/libopencv_core.a'):
    path = session/name
    path.parent.mkdir(parents=True, exist_ok=True)
    path.touch()
source = Path(os.environ['ROOT_PROJECT'])/'third_party/opencv/source.cpp'
source.parent.mkdir(parents=True, exist_ok=True)
source.touch()
print(session)
raise SystemExit(int(os.environ['TEST_EXIT_STATUS']))
"""
        for status in (0, 7):
            with self.subTest(status=status):
                result = self.session(code, dict(self.env, XR_KEEP_OPENCV_BUILD="0",
                                                TEST_EXIT_STATUS=str(status)))
                self.assertEqual(result.returncode, status, result.stderr)
                self.assertFalse(Path(result.stdout.strip()).exists())
                self.assertTrue((self.root / "third_party/opencv/source.cpp").exists())

    def test_invalid_opencv_retention_stops_before_build_command(self):
        result = self.session("raise AssertionError('build must not start')",
                              dict(self.env, XR_KEEP_OPENCV_BUILD="invalid"))
        self.assertEqual(result.returncode, 2)
        self.assertIn("XR_KEEP_OPENCV_BUILD must be 0 or 1", result.stderr)
        self.assertFalse((self.root / "build").exists())

    def opencv_source_fixture(self):
        upstream = self.root / "upstream"
        upstream.mkdir()
        subprocess.run(["git", "init", "-q", str(upstream)], check=True)
        generator = upstream / "cmake/OpenCVGenPkgconfig.cmake"
        generator.parent.mkdir()
        generator.write_text("cmake_minimum_required(VERSION 2.8.12.2)\n")
        subprocess.run(["git", "-C", str(upstream), "add", "."], check=True)
        subprocess.run(["git", "-C", str(upstream), "-c", "user.name=Test",
                        "-c", "user.email=test@example.com", "commit", "-qm", "Fixture"], check=True)
        commit = subprocess.check_output(["git", "-C", str(upstream), "rev-parse", "HEAD"], text=True).strip()
        return upstream, commit

    @unittest.skipUnless(shutil.which("git"), "Git needed")
    def test_opencv_source_checkout_and_cmake_patch_are_reused_without_network(self):
        upstream, commit = self.opencv_source_fixture()
        source = self.root / "third_party/opencv"
        command = ["bash", str(BUILD / "prepare_opencv_source.sh"), str(source), commit, str(upstream)]
        first = subprocess.run(command, capture_output=True, text=True)
        self.assertEqual(first.returncode, 0, first.stderr)
        generator = source / "cmake/OpenCVGenPkgconfig.cmake"
        patched = generator.read_bytes()
        self.assertIn(b"cmake_minimum_required(VERSION 3.10)", patched)
        # A second invocation must work even when the upstream is unavailable.
        shutil.rmtree(upstream)
        second = subprocess.run(command, capture_output=True, text=True)
        self.assertEqual(second.returncode, 0, second.stderr)
        self.assertEqual(generator.read_bytes(), patched)
        self.assertEqual(subprocess.check_output(["git", "-C", str(source), "rev-parse", "HEAD"],
                                                text=True).strip(), commit)

    def test_opencv_source_preparation_preserves_non_git_directory(self):
        source = self.root / "third_party/opencv"
        source.mkdir(parents=True)
        existing = source / "source.cpp"
        existing.write_text("developer source\n")
        result = subprocess.run(["bash", str(BUILD / "prepare_opencv_source.sh"), str(source),
                                 "71d3237a093b60a27601c20e9ee6c3e52154e8b1"],
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, 2, result.stderr)
        self.assertIn("nonempty source directory", result.stderr)
        self.assertEqual(existing.read_text(), "developer source\n")
        self.assertFalse((source / ".git").exists())

    @unittest.skipUnless(shutil.which("git"), "Git needed")
    def test_failed_opencv_build_keeps_third_party_sources_and_build_products(self):
        upstream, commit = self.opencv_source_fixture()
        source = self.root / "third_party/opencv"
        subprocess.run(["git", "clone", "-q", str(upstream), str(source)], check=True)
        scripts = self.root / "test scripts"
        scripts.mkdir()
        # Only replace the pinned revision with our local fixture; exercise the
        # real builder and session cleanup without downloading/compiling OpenCV.
        builder = scripts / "build_opencv.sh"
        builder.write_text((BUILD / builder.name).read_text().replace(
            "71d3237a093b60a27601c20e9ee6c3e52154e8b1", commit))
        shutil.copy2(BUILD / "prepare_opencv_source.sh", scripts)
        cmake = scripts / "cmake"
        cmake.write_text("#!/usr/bin/env python3\n"
                         "import os, sys\nfrom pathlib import Path\n"
                         "build = Path(sys.argv[sys.argv.index('-B') + 1])\n"
                         "build.mkdir(parents=True)\n(build/'CMakeCache.txt').touch()\n"
                         "Path(os.environ['TEST_SESSION_PATH']).write_text(os.environ['XR_BUILD_SESSION_DIR'])\n"
                         "raise SystemExit(9)\n")
        cmake.chmod(0o755)
        session_path = self.root / "session-path"
        result = self.session("import os, subprocess; raise SystemExit(subprocess.call(['bash', os.environ['TEST_BUILDER']]))",
                              dict(self.env, PATH=str(scripts) + os.pathsep + self.env['PATH'],
                                   TEST_BUILDER=str(builder), TEST_SESSION_PATH=str(session_path)))
        self.assertEqual(result.returncode, 9, result.stderr)
        session = Path(session_path.read_text())
        self.assertTrue((session / "opencv/build/CMakeCache.txt").exists())
        self.assertTrue((session / "opencv/install").is_dir())
        self.assertTrue((source / "cmake/OpenCVGenPkgconfig.cmake").exists())
        self.assertFalse((session / "opencv/source").exists())
        self.assertEqual({path.name for path in session.iterdir()}, {"opencv"})

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
cpu_tag=os.environ['XR_CPU_CACHE_TAG']
expected=f"{token}|{release['ID']}:{release['VERSION_ID']}|71d3237a093b60a27601c20e9ee6c3e52154e8b1|{cpu_tag}"
(prefix/'XR_BUILD_ID').write_text(expected+'\n')
env=dict(os.environ,XR_OPENCV_PREFIX=str(prefix))
script=os.environ['TEST_OPENCV_SCRIPT']
for _ in range(2):
    r=subprocess.run(['bash',script],env=env,capture_output=True,text=True)
    assert r.returncode==0,r.stderr
for replacement in ['ci:123:2',expected.replace(cpu_tag,'another-cpu'),expected.replace(release['VERSION_ID'],'0.0')]:
    (prefix/'XR_BUILD_ID').write_text(replacement+'\n')
    r=subprocess.run(['bash',script],env=env,capture_output=True,text=True)
    assert r.returncode==2,r.stderr
"""
        env = dict(self.env, GITHUB_RUN_ID="123", GITHUB_RUN_ATTEMPT="1",
                   TEST_OPENCV_SCRIPT=str(BUILD / "build_opencv.sh"))
        for profile in ("avx2", "generic"):
            with self.subTest(profile=profile):
                result = self.session(code, dict(env, XR_BUILD_CPU=profile))
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
        for mode, march, tune, opencv in [("GeNeRiC", "x86-64", "generic", "SSE2"),
                                         ("AVX2", "x86-64-v3", "generic", "AVX2"),
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

    @unittest.skipUnless(shutil.which("gcc"), "GCC needed")
    def test_generic_compiler_uses_sse2_without_avx_or_newer_sse(self):
        code = r'''
import os, shlex, subprocess
from pathlib import Path
source = """
#if !defined(__x86_64__) || !defined(__SSE2__)
#error x86-64 and SSE2 are required
#endif
#if defined(__AVX__) || defined(__AVX2__) || defined(__AVX512F__) || defined(__FMA__) || defined(__SSE3__) || defined(__SSSE3__) || defined(__SSE4_1__) || defined(__SSE4_2__)
#error generic must not require instructions above the x86-64 baseline
#endif
int main(void) { return 0; }
"""
binary = Path(os.environ['XR_BUILD_SESSION_DIR']) / 'generic-smoke'
subprocess.run(['gcc', *shlex.split(os.environ['CFLAGS']), '-x', 'c', '-', '-o', str(binary)],
               input=source, text=True, check=True)
subprocess.run([str(binary)], check=True)
'''
        # A host-native architecture flag must be overridden by the profile.
        result = self.session(code, dict(self.env, XR_BUILD_CPU="generic", CFLAGS="-march=native"))
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_invalid_profile_stops_before_build_command(self):
        result = self.session("raise AssertionError('build must not start')", dict(self.env, XR_BUILD_CPU="invalid"))
        self.assertEqual(result.returncode, 2)
        self.assertIn("XR_BUILD_CPU must be", result.stderr)
        self.assertEqual(list((self.root / "build/.sessions").iterdir()), [])

    def test_local_output_directories_are_separate(self):
        for profile, suffix in [("GeNeRiC", "out/generic/xr-gate"),
                                ("avx2", "out/xr-gate"), ("AvX512", "out/avx512/xr-gate"),
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
        fresh.write_text(json.dumps(module.describe("generic")))
        with self.assertRaisesRegex(ValueError, "another CPU profile"):
            module.check_output(self.root, fresh)
        (self.root / "BUILD_CPU_TARGET").write_text("x86-64\n")
        (self.root / "BUILD_CPU_PROFILE.json").write_text(json.dumps(module.describe("generic")))
        module.check_output(self.root, fresh)
        fresh.write_text(json.dumps(module.describe("avx2")))
        with self.assertRaisesRegex(ValueError, "another CPU profile"):
            module.check_output(self.root, fresh)

    def test_generic_runtime_accepts_baseline_cpu_and_rejects_missing_sse2(self):
        spec = importlib.util.spec_from_file_location("cpu_target", BUILD.parent / "runtime/cpu_target.py")
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        info = lambda flags: "flags : " + " ".join(sorted(flags)) + "\n"
        generic = module.describe("generic", info(module.V1_FLAGS))
        self.assertEqual(set(generic["required_features"]), module.V1_FLAGS)
        self.assertFalse({"avx", "avx2", "fma", "sse3", "ssse3", "sse4_1", "sse4_2"}
                         & set(generic["required_features"]))
        marker = self.root / "BUILD_CPU_TARGET"
        marker.write_text("x86-64\n")
        module.check_package(self.root, info(module.V1_FLAGS))
        module.check_package(self.root, info(module.V4_FLAGS))
        with self.assertRaisesRegex(ValueError, "sse2"):
            module.check_package(self.root, info(module.V1_FLAGS - {"sse2"}))
        marker.write_text("x86-64-v3\n")
        with self.assertRaisesRegex(ValueError, "missing CPU features"):
            module.check_package(self.root, info(module.V1_FLAGS))

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
