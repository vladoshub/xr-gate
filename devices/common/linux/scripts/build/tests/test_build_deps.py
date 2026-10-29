#!/usr/bin/env python3
"""Exercise preparation and unprivileged build dispatch without installing packages."""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

BUILD = Path(__file__).resolve().parents[1]
ROOT = BUILD.parents[4]
PREPARE = BUILD / "install_build_deps_ubuntu.sh"


class BuildDependencyTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.project = self.root / "project with spaces"
        self.project.mkdir()
        self.bin = self.root / "bin"
        self.bin.mkdir()
        self.log = self.root / "commands.jsonl"
        self.env = dict(os.environ, ROOT_PROJECT=str(self.project), XR_ROOT_PROJECT=str(self.project),
                        PATH=str(self.bin) + os.pathsep + os.environ["PATH"], TEST_COMMAND_LOG=str(self.log))
        for name in ("XR_BUILD_ONLY", "XR_BUILD_SESSION_DIR", "XR_BUILD_SESSION_OWNER", "XR_OPENCV_PREFIX",
                     "XR_DEVICE_OUT_ENV", "XR_DEVICE_BUILD_HOOK", "XR_DEVICE_PACKAGE_SCRIPT", "XR_OUT_ROOT",
                     "XR_OUT_BIN_ROOT", "XR_BIN_ROOT", "GITHUB_RUN_ID", "GITHUB_RUN_ATTEMPT", "XR_LOCAL_BUILD_ID",
                     "XR_BUILD_XRIZER", "XR_DOWNLOAD_HAND_TRACKING_MODELS", "XR_DOWNLOAD_MERCURY_MODELS",
                     "XR_BUILD_VENDOR_COMPONENTS", "XR_VENDOR_COMPONENTS"):
            self.env.pop(name, None)
        self.executable(self.bin / "dpkg-query", """#!/usr/bin/env python3
import os, sys
for package in sys.argv[1:]:
    if not package.startswith('-'):
        status = 'not-installed' if package == os.environ.get('TEST_MISSING_PACKAGE') else 'installed'
        print(package + '\\t' + status)
""")
        for command in ("sudo", "apt", "apt-get", "usermod"):
            self.executable(self.bin / command, """#!/usr/bin/env python3
import json, os, sys
with open(os.environ['TEST_COMMAND_LOG'], 'a') as out:
    out.write(json.dumps({'forbidden': sys.argv}) + '\\n')
raise SystemExit(91)
""")

    def executable(self, path, text):
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text)
        path.chmod(0o755)

    def run_script(self, path, *args, **overrides):
        return subprocess.run(["bash", str(path), *args], env=dict(self.env, **overrides),
                              capture_output=True, text=True)

    def packages(self, **env):
        result = self.run_script(PREPARE, "--list", **env)
        self.assertEqual(result.returncode, 0, result.stderr)
        packages = result.stdout.splitlines()
        self.assertEqual(len(packages), len(set(packages)))
        return set(packages)

    def test_package_selection_follows_partial_targets_and_optional_components(self):
        models = self.packages(XR_BUILD_ONLY="models")
        self.assertIn("git-lfs", models)
        self.assertNotIn("build-essential", models)
        self.assertNotIn("libvulkan-dev", models)
        drivers = self.packages(XR_BUILD_ONLY="drivers")
        self.assertTrue({"cmake", "libvulkan-dev", "libsystemd-dev", "libgl-dev"} <= drivers)
        self.assertNotIn("clang", drivers)
        self.assertNotIn("clang", self.packages(XR_BUILD_ONLY="xr_spatial", XR_BUILD_XRIZER="1"))
        optional = self.packages(XR_BUILD_ONLY="drivers_with_xrizer")
        self.assertTrue({"clang", "libclang-dev", "glslc"} <= optional)
        self.assertNotIn("git-lfs", self.packages())
        self.assertIn("git-lfs", self.packages(XR_DOWNLOAD_HAND_TRACKING_MODELS="1"))
        helper = self.packages(XR_BUILD_ONLY="xreal_display_helper")
        self.assertIn("libhidapi-dev", helper)
        self.assertNotIn("libhidapi-dev", self.packages(XR_BUILD_ONLY="xreal_display_helper",
                                                       XR_BUILD_VENDOR_COMPONENTS="0"))

    def test_check_reports_missing_dependencies_without_sudo_or_apt(self):
        result = self.run_script(PREPARE, "--check", TEST_MISSING_PACKAGE="libhidapi-dev")
        self.assertEqual(result.returncode, 2, result.stderr)
        self.assertIn("libhidapi-dev", result.stderr)
        self.assertIn("install_build_deps_ubuntu.sh", result.stderr)
        self.assertFalse(self.log.exists())
        installed = self.run_script(PREPARE, "--check")
        self.assertEqual(installed.returncode, 0, installed.stderr)
        self.assertFalse(self.log.exists())

    def test_preparation_uses_one_sudo_process_and_stops_if_apt_update_fails(self):
        self.executable(self.bin / "id", "#!/bin/sh\necho 1000\n")
        self.executable(self.bin / "sudo", """#!/usr/bin/env bash
echo '{"sudo": true}' >> "$TEST_COMMAND_LOG"
exec "$@"
""")
        self.executable(self.bin / "apt-get", """#!/usr/bin/env python3
import json, os, sys
with open(os.environ['TEST_COMMAND_LOG'], 'a') as out:
    out.write(json.dumps({'apt': sys.argv[1:], 'frontend': os.environ.get('DEBIAN_FRONTEND')}) + '\\n')
if sys.argv[1] == 'update' and os.environ.get('TEST_APT_FAIL'):
    raise SystemExit(23)
""")
        result = self.run_script(PREPARE, XR_BUILD_ONLY="models")
        self.assertEqual(result.returncode, 0, result.stderr)
        commands = [json.loads(line) for line in self.log.read_text().splitlines()]
        self.assertEqual(sum('sudo' in command for command in commands), 1)
        apt = [command for command in commands if 'apt' in command]
        self.assertEqual(apt[0]['apt'], ['update'])
        self.assertEqual(apt[1]['apt'][:3], ['install', '-y', '--no-install-recommends'])
        self.assertIn('git-lfs', apt[1]['apt'])
        self.assertEqual(apt[1]['frontend'], 'noninteractive')
        self.log.unlink()
        failed = self.run_script(PREPARE, XR_BUILD_ONLY="models", TEST_APT_FAIL="1")
        self.assertEqual(failed.returncode, 23, failed.stderr)
        commands = [json.loads(line) for line in self.log.read_text().splitlines()]
        self.assertEqual([command['apt'] for command in commands if 'apt' in command], [['update']])

    def stub_build(self):
        text = """#!/usr/bin/env bash
python3 - "$0" "$@" <<'PY'
import json, os, sys
names = ['INSTALL_APT_DEPS', 'INSTALL_LFS_DEPS', 'XRIZER_INSTALL_SYSTEM_DEPS', 'ADD_INPUT_GROUP']
with open(os.environ['TEST_COMMAND_LOG'], 'a') as out:
    out.write(json.dumps({'step': sys.argv[1], 'args': sys.argv[2:],
                          'flags': {name: os.environ.get(name) for name in names}}) + '\\n')
PY
"""
        scripts = [
            "capture_service_cpp/scripts/linux/build_capture_service_cpp.sh",
            "backends/basalt_vio/scripts/linux/install_basalt.sh",
            "backends/imu_3dof/scripts/linux/install_imu_3dof.sh",
            "backends/mercury_hand_tracking/scripts/linux/install_mercury.sh",
            "backends/mercury_hand_tracking/scripts/linux/download_mercury_models.sh",
            "runtime_adapters/xr_runtime_adapter/scripts/linux/install_xr_runtime_adapter.sh",
            "override_controller/scripts/linux/install_override_controller.sh",
            "backends/xr_video/scripts/linux/install_xr_video.sh",
            "backends/xr_spatial/scripts/linux/install_xr_spatial.sh",
            "bridges/scripts/linux/install_bridges.sh",
            "drivers/build_profiles.sh", "drivers/xrizer/scripts/linux/install_xrizer.sh",
            "apps/steamvr/video_overlay/scripts/linux/install_steamvr_video_overlay.sh",
            "apps/steamvr/spatial_overlay/scripts/linux/install_xr_steamvr_spatial_overlay.sh",
            "apps/steamvr/spatial_scene/scripts/linux/install_xr_steamvr_spatial_scene.sh",
            "vendor-hook.sh", "package-hook.sh",
        ]
        for relative in scripts:
            self.executable(self.project / relative, text)
        self.env.update(XR_BUILD_CPU="avx2", XR_USE_BUNDLED_OPENCV="0",
                        XR_DEVICE_BUILD_HOOK=str(self.project / "vendor-hook.sh"),
                        XR_DEVICE_PACKAGE_SCRIPT=str(self.project / "package-hook.sh"),
                        XR_BUILD_XRIZER="1", XR_DOWNLOAD_HAND_TRACKING_MODELS="1")

    def test_full_build_disables_privileged_setup_for_all_children(self):
        self.stub_build()
        result = self.run_script(BUILD / "install_xr_gate_out.sh", INSTALL_APT_DEPS="1",
                                 INSTALL_LFS_DEPS="1", XRIZER_INSTALL_SYSTEM_DEPS="1", ADD_INPUT_GROUP="1")
        self.assertEqual(result.returncode, 0, result.stderr)
        commands = [json.loads(line) for line in self.log.read_text().splitlines()]
        self.assertEqual(len(commands), 18)
        for command in commands:
            self.assertNotIn('forbidden', command)
            self.assertEqual(set(command['flags'].values()), {'0'}, command)
        self.assertTrue(any(command['step'].endswith('download_mercury_models.sh') for command in commands))
        self.assertTrue(any(command['step'].endswith('install_xrizer.sh') for command in commands))

    def test_missing_dependencies_stop_build_before_any_compilation(self):
        self.stub_build()
        result = self.run_script(BUILD / "install_xr_gate_out.sh", TEST_MISSING_PACKAGE="libhidapi-dev")
        self.assertEqual(result.returncode, 2, result.stderr)
        self.assertIn('libhidapi-dev', result.stderr)
        self.assertFalse(self.log.exists())

    def test_standalone_xreal_helpers_never_install_packages(self):
        self.executable(self.bin / "cmake", """#!/usr/bin/env python3
import sys
from pathlib import Path
if sys.argv[1] == '--build':
    build = Path(sys.argv[2])
    build.mkdir(parents=True, exist_ok=True)
    (build/'xreal_display_helper').write_text('fixture binary')
""")
        for relative in ("tools/xreal_ultra/xreal_display_helper/scripts/linux/install_xreal_helper.sh",
                         "runtime_adapters/xr_runtime_adapter/scripts/linux/install_xreal_helper.sh"):
            with self.subTest(script=relative):
                # Legacy setup flags must not reintroduce package installs.
                result = self.run_script(ROOT / relative, INSTALL_APT_DEPS="1")
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertTrue((self.project / "bin/xreal_display_helper/xreal_display_helper").exists())
                self.assertFalse(self.log.exists())

    def test_xrizer_accepts_prepared_libclang_without_package_installation(self):
        source = self.project / "third_party/xrizer"
        source.mkdir(parents=True)
        (source / "Cargo.toml").touch()
        libclang = self.root / "libclang"
        libclang.mkdir()
        (libclang / "libclang.so").touch()
        for tool in ('gcc', 'pkg-config', 'clang', 'glslc'):
            self.executable(self.bin / tool, '#!/bin/sh\nexit 0\n')
        self.executable(self.bin / 'ldconfig', '#!/bin/sh\nexit 1\n')
        self.executable(self.bin / 'cargo', """#!/usr/bin/env bash
if [[ "$*" == 'xbuild --version' ]]; then exit 0; fi
echo 'REACHED_XRIZER_COMPILATION' >&2
exit 37
""")
        result = self.run_script(ROOT / 'drivers/xrizer/scripts/linux/install_xrizer.sh',
                                 XRIZER_DIR=str(source), CLONE_XRIZER='0', LIBCLANG_PATH=str(libclang),
                                 XRIZER_INSTALL_RUST_SRC='0', XRIZER_INSTALL_CARGO_XBUILD='0')
        self.assertEqual(result.returncode, 37, result.stderr)
        self.assertIn('REACHED_XRIZER_COMPILATION', result.stderr)
        self.assertFalse(self.log.exists())


if __name__ == "__main__":
    unittest.main()
