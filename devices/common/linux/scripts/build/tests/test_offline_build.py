#!/usr/bin/env python3
"""Exercise online preparation, offline rebuilds and explicit refresh locally."""
import ctypes.util
import http.server
import io
import json
import os
from pathlib import Path
import shutil
import socket
import subprocess
import sys
import tarfile
import tempfile
import threading
import unittest

BUILD = Path(__file__).resolve().parents[1]
ROOT = BUILD.parents[4]


class OfflineBuildTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name) / "project with spaces"
        self.root.mkdir()
        self.env = dict(os.environ, ROOT_PROJECT=str(self.root), XR_BUILD_CPU="generic")
        for name in ("XR_BUILD_SESSION_DIR", "XR_BUILD_SESSION_OWNER", "XR_LOCAL_BUILD_ID",
                     "GITHUB_RUN_ID", "GITHUB_RUN_ATTEMPT", "XR_OFFLINE", "XR_UPDATE_DEPS",
                     "XR_DEPENDENCY_CACHE_ROOT", "XR_OPENCV_PREFIX", "XR_BUILD_ONLY"):
            self.env.pop(name, None)

    def run_session(self, command, **env):
        return subprocess.run(["bash", str(BUILD / "with_build_session.sh"), *command],
                              env=dict(self.env, **env), capture_output=True, text=True)

    def git(self, directory, *args):
        return subprocess.check_output(["git", "-C", str(directory), *args], text=True,
                                       stderr=subprocess.DEVNULL).strip()

    def repo(self, directory):
        directory.mkdir(parents=True)
        self.git(directory, "init", "-q")
        self.git(directory, "config", "user.name", "Test")
        self.git(directory, "config", "user.email", "test@example.invalid")
        (directory / "main.c").write_text('#include "dep.h"\nint main(void) {return VALUE == 42 ? 0 : 1;}\n')
        self.git(directory, "add", ".")
        self.git(directory, "commit", "-qm", "initial")
        return self.git(directory, "rev-parse", "HEAD")

    def server(self):
        payload = io.BytesIO()
        with tarfile.open(fileobj=payload, mode="w:gz") as archive:
            data = b"#define VALUE 42\n"
            entry = tarfile.TarInfo("dep.h")
            entry.size = len(data)
            archive.addfile(entry, io.BytesIO(data))
        requests = []

        class Handler(http.server.BaseHTTPRequestHandler):
            def do_GET(self):
                requests.append(self.path)
                self.send_response(200)
                self.end_headers()
                self.wfile.write(payload.getvalue())

            def log_message(self, *args):
                pass

        server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        self.addCleanup(server.server_close)
        self.addCleanup(server.shutdown)
        return f"http://127.0.0.1:{server.server_port}/headers.tar.gz", requests

    @unittest.skipUnless(shutil.which("cc") and ctypes.util.find_library("seccomp"), "Linux compiler and libseccomp needed")
    def test_rebuild_compiles_from_cached_sources_and_archive_with_network_disabled(self):
        upstream = self.root / "upstream"
        commit = self.repo(upstream)
        url, requests = self.server()
        worker = self.root / "build.sh"
        worker.write_text('''#!/usr/bin/env bash
set -euo pipefail
source "$TEST_POLICY"
ref="$(xr_ensure_git_source "$ROOT_PROJECT/third_party/example" "$TEST_UPSTREAM" "$TEST_REF")"
git -C "$ROOT_PROJECT/third_party/example" checkout --quiet "$ref"
xr_update_submodules "$ROOT_PROJECT/third_party/example"
xr_download "$TEST_URL" "$XR_DEPENDENCY_CACHE_ROOT/downloads/headers.tar.gz"
mkdir -p "$XR_BUILD_ROOT/deps"
tar -xzf "$XR_DEPENDENCY_CACHE_ROOT/downloads/headers.tar.gz" -C "$XR_BUILD_ROOT/deps"
cc -I"$XR_BUILD_ROOT/deps" "$ROOT_PROJECT/third_party/example/main.c" -o "$XR_BUILD_ROOT/test"
"$XR_BUILD_ROOT/test"
python3 - <<'PY'
import json, os, socket
if os.environ['XR_OFFLINE'] == '1':
    for family in (socket.AF_INET, socket.AF_INET6):
        try:
            socket.socket(family, socket.SOCK_STREAM)
        except OSError:
            pass
        else:
            raise AssertionError('offline build permitted an internet socket')
    if os.environ.get('TEST_UNIX_SOCKET_SUPPORTED') == '1':
        socket.socket(socket.AF_UNIX).close()
print(json.dumps({k:os.environ[k] for k in ['XR_OFFLINE','XR_BUILD_ROOT','VCPKG_DOWNLOADS','CCACHE_DIR','XR_VCPKG_INSTALLED_DIR']}))
PY
''')
        self.env.update(TEST_POLICY=str(BUILD / "dependency_policy.sh"), TEST_UPSTREAM=str(upstream),
                        TEST_REF=commit, TEST_URL=url)
        try:
            socket.socket(socket.AF_UNIX).close()
        except OSError:
            pass  # Some test runners already disallow Unix sockets.
        else:
            self.env['TEST_UNIX_SOCKET_SUPPORTED'] = '1'
        first = self.run_session(["bash", str(worker)])
        self.assertEqual(first.returncode, 0, first.stderr)
        one = json.loads(first.stdout)
        self.assertEqual(one["XR_OFFLINE"], "0")
        self.assertEqual(requests, ["/headers.tar.gz"])
        # The origin becomes unavailable. The second invocation still performs
        # a real compile in a new build tree, without consulting the origin.
        upstream.rename(self.root / "origin unavailable")
        second = self.run_session(["bash", str(worker)])
        self.assertEqual(second.returncode, 0, second.stderr)
        two = json.loads(second.stdout)
        self.assertEqual(two["XR_OFFLINE"], "1")
        self.assertNotEqual(one["XR_BUILD_ROOT"], two["XR_BUILD_ROOT"])
        for name in ("VCPKG_DOWNLOADS", "CCACHE_DIR", "XR_VCPKG_INSTALLED_DIR"):
            self.assertEqual(one[name], two[name])
        self.assertEqual(len(requests), 1)
        # Even keeping no OpenCV build must leave dependency downloads intact.
        third = self.run_session(["bash", str(worker)], XR_KEEP_OPENCV_BUILD="0")
        self.assertEqual(third.returncode, 0, third.stderr)
        self.assertTrue((self.root / "build/.dependency-cache/downloads/headers.tar.gz").is_file())
        # Missing cache entries fail locally and suggest the recovery command.
        (self.root / "build/.dependency-cache/downloads/headers.tar.gz").unlink()
        missing = self.run_session(["bash", str(worker)])
        self.assertEqual(missing.returncode, 2, missing.stderr)
        self.assertIn("XR_OFFLINE=0", missing.stderr)
        self.assertEqual(len(requests), 1)

    def test_refresh_is_explicit_and_does_not_advance_the_pinned_ref(self):
        upstream = self.root / "upstream"
        commit = self.repo(upstream)
        source = self.root / "third_party/example"
        script = 'source "$TEST_POLICY"; ref=$(xr_ensure_git_source "$TEST_SOURCE" "$TEST_UPSTREAM" "$TEST_REF") || exit $?; git -C "$TEST_SOURCE" checkout --quiet "$ref"'
        self.env.update(TEST_POLICY=str(BUILD / "dependency_policy.sh"), TEST_UPSTREAM=str(upstream),
                        TEST_SOURCE=str(source), TEST_REF=commit)
        command = ["bash", "-euc", script]
        first = self.run_session(command)
        self.assertEqual(first.returncode, 0, first.stderr)
        (upstream / "new-file").touch()
        self.git(upstream, "add", ".")
        self.git(upstream, "commit", "-qm", "remote changed")
        new = self.git(upstream, "rev-parse", "HEAD")
        cached = self.run_session(command, XR_OFFLINE="0")
        self.assertEqual(cached.returncode, 0, cached.stderr)
        self.assertNotEqual(subprocess.run(["git", "-C", str(source), "cat-file", "-e", new],
                                          capture_output=True).returncode, 0)
        updated = subprocess.run(["bash", str(BUILD / "update_build_dependencies.sh"), *command],
                                 env=self.env, capture_output=True, text=True)
        self.assertEqual(updated.returncode, 0, updated.stderr)
        self.assertEqual(self.git(source, "rev-parse", "HEAD"), commit)
        self.assertEqual(self.git(source, "rev-parse", "origin/HEAD"), new)

    def test_readiness_is_scoped_to_configuration_and_failed_runs_do_not_mark_ready(self):
        command = [sys.executable, "-c", "import os; print(os.environ['XR_OFFLINE']); raise SystemExit(int(os.environ.get('TEST_STATUS',0)))"]
        for status in (7, 7, 0, 0):
            result = self.run_session(command, TEST_STATUS=str(status))
            self.assertEqual(result.returncode, status, result.stderr)
            self.assertEqual(result.stdout.strip(), "1" if status == 0 and getattr(self, "succeeded", False) else "0")
            self.succeeded = status == 0
        changed = self.run_session(command, XR_BUILD_ONLY="models")
        self.assertEqual(changed.stdout.strip(), "0", changed.stderr)
        changed = self.run_session(command, BASALT_REF="another-ref")
        self.assertEqual(changed.stdout.strip(), "0", changed.stderr)
        # An unsuccessful refresh invalidates the old successful marker.
        failed = self.run_session(command, XR_UPDATE_DEPS="1", TEST_STATUS="9")
        self.assertEqual(failed.returncode, 9, failed.stderr)
        again = self.run_session(command)
        self.assertEqual(again.stdout.strip(), "0", again.stderr)

    def test_ci_remains_isolated_and_invalid_policy_stops_before_the_command(self):
        code = "import os; from pathlib import Path; print(os.environ['XR_OFFLINE']); print(os.environ['XR_DEPENDENCY_CACHE_ROOT'])"
        for _ in range(2):
            result = self.run_session([sys.executable, "-c", code], GITHUB_RUN_ID="123", GITHUB_RUN_ATTEMPT="1")
            self.assertEqual(result.returncode, 0, result.stderr)
            mode, cache = result.stdout.splitlines()
            self.assertEqual(mode, "0")
            self.assertFalse(Path(cache).exists())
        for env in ({"XR_OFFLINE": "invalid"}, {"XR_UPDATE_DEPS": "1", "XR_OFFLINE": "1"}):
            result = self.run_session([sys.executable, "-c", "raise AssertionError('must not execute')"], **env)
            self.assertEqual(result.returncode, 2, result.stderr)
            self.assertNotIn("AssertionError", result.stderr)

    def test_submodules_reuse_local_objects_and_missing_sources_fail_offline(self):
        upstream = self.root / "upstream"
        self.repo(upstream)
        module = self.root / "module"
        self.repo(module)
        self.git(upstream, "-c", "protocol.file.allow=always", "submodule", "add", "-q", str(module), "nested")
        self.git(upstream, "commit", "-qam", "submodule")
        commit = self.git(upstream, "rev-parse", "HEAD")
        self.env.update(TEST_POLICY=str(BUILD / "dependency_policy.sh"), TEST_UPSTREAM=str(upstream), TEST_REF=commit,
                        GIT_CONFIG_COUNT="1", GIT_CONFIG_KEY_0="protocol.file.allow", GIT_CONFIG_VALUE_0="always")
        code = 'source "$TEST_POLICY"; xr_ensure_git_source "$ROOT_PROJECT/source" "$TEST_UPSTREAM" "$TEST_REF" >/dev/null; xr_update_submodules "$ROOT_PROJECT/source"'
        first = self.run_session(["bash", "-euc", code])
        self.assertEqual(first.returncode, 0, first.stderr)
        module.rename(self.root / "module unavailable")
        offline = self.run_session(["bash", "-euc", code])
        self.assertEqual(offline.returncode, 0, offline.stderr)
        missing = self.run_session(["bash", "-euc", 'source "$TEST_POLICY"; xr_ensure_git_source "$ROOT_PROJECT/missing" "$TEST_UPSTREAM" "$TEST_REF"'], XR_OFFLINE="1")
        self.assertEqual(missing.returncode, 2, missing.stderr)
        self.assertIn("source checkout missing", missing.stderr)

    def test_shallow_pinned_sources_are_reusable_and_non_git_directories_are_preserved(self):
        upstream = self.root / "upstream"
        commit = self.repo(upstream)
        self.env.update(TEST_POLICY=str(BUILD / "dependency_policy.sh"), TEST_UPSTREAM=str(upstream), TEST_REF=commit)
        code = 'source "$TEST_POLICY"; ref=$(xr_ensure_git_source "$ROOT_PROJECT/source" "$TEST_UPSTREAM" "$TEST_REF" 1 "$XR_UPDATE_DEPS" 1); git -C "$ROOT_PROJECT/source" checkout --quiet "$ref"'
        first = self.run_session(["bash", "-euc", code])
        self.assertEqual(first.returncode, 0, first.stderr)
        self.assertEqual(self.git(self.root / "source", "rev-parse", "--is-shallow-repository"), "true")
        upstream.rename(self.root / "upstream unavailable")
        second = self.run_session(["bash", "-euc", code])
        self.assertEqual(second.returncode, 0, second.stderr)
        self.assertEqual(self.git(self.root / "source", "rev-parse", "HEAD"), commit)
        custom = self.root / "custom"
        custom.mkdir()
        (custom / "developer.cpp").write_text("preserve me")
        rejected = self.run_session(["bash", "-euc", 'source "$TEST_POLICY"; xr_ensure_git_source "$ROOT_PROJECT/custom" "$TEST_UPSTREAM" "$TEST_REF" 1 0 1'])
        self.assertEqual(rejected.returncode, 2, rejected.stderr)
        self.assertIn("nonempty source directory", rejected.stderr)
        self.assertEqual((custom / "developer.cpp").read_text(), "preserve me")

    @unittest.skipUnless(shutil.which("git-lfs"), "Git LFS needed")
    def test_optional_models_hydrate_from_local_lfs_objects_on_rebuild(self):
        upstream = self.root / "models upstream"
        self.repo(upstream)
        self.git(upstream, "lfs", "install", "--local")
        self.git(upstream, "lfs", "track", "*.onnx")
        relative = "src/steamvr_driver/mercury/resources/internal/hand-tracking-models"
        model_dir = upstream / relative
        model_dir.mkdir(parents=True)
        names = ["grayscale_detection_160x160.onnx", "grayscale_keypoint_jan18.onnx"]
        for name in names:
            (model_dir / name).write_bytes(b"fixture model binary\0" * 100)
        (upstream / "LICENSES").mkdir()
        (upstream / "LICENSES/BSL-1.0.txt").write_text("fixture license")
        self.git(upstream, "add", ".")
        self.git(upstream, "commit", "-qm", "models")
        commit = self.git(upstream, "rev-parse", "HEAD")
        self.env.update(MERCURY_DRIVER_REPO_URL=str(upstream), MERCURY_DRIVER_REF=commit)
        script = ROOT / "backends/mercury_hand_tracking/scripts/linux/download_mercury_models.sh"
        first = self.run_session(["bash", str(script)])
        self.assertEqual(first.returncode, 0, first.stderr)
        source = self.root / "third_party/mercury_steamvr_driver"
        self.assertTrue(list((source / ".git/lfs/objects").rglob("*")))
        upstream.rename(self.root / "models origin unavailable")
        second = self.run_session(["bash", str(script)])
        self.assertEqual(second.returncode, 0, second.stderr)
        self.assertIn("offline=1", second.stderr)
        for name in names:
            self.assertEqual((self.root / "bin/hand-tracking-models/mercury" / name).read_bytes(),
                             b"fixture model binary\0" * 100)
        # Runtime packages omit build scripts. The downloader must find the
        # policy helper beside itself after the assembled package is moved.
        packaged = self.root / "moved runtime/bin/scripts/backends/mercury_hand_tracking"
        packaged.mkdir(parents=True)
        shutil.copy2(script, packaged)
        shutil.copy2(BUILD / "dependency_policy.sh", packaged)
        relocated = self.run_session(["bash", str(packaged / script.name)], XR_OFFLINE="1")
        self.assertEqual(relocated.returncode, 0, relocated.stderr)
        shutil.rmtree(source / ".git/lfs/objects")
        for name in names:
            (source / relative / name).unlink()
        missing = self.run_session(["bash", str(script)])
        self.assertEqual(missing.returncode, 2, missing.stderr)
        self.assertIn("LFS objects missing", missing.stderr)

    @unittest.skipUnless(shutil.which("rsync"), "rsync needed for source compliance packaging")
    def test_xrizer_uses_prepared_toolchain_and_offline_cargo_for_build_and_vendor(self):
        source = self.root / "third_party/xrizer"
        commit = self.repo(source)
        for name in ("Cargo.toml", "Cargo.lock", "LICENSE"):
            (source / name).write_text("fixture source")
        self.git(source, "add", ".")
        self.git(source, "commit", "-qm", "Rust source")
        commit = self.git(source, "rev-parse", "HEAD")
        for relative in ("drivers/xrizer/scripts/linux/install_xrizer.sh",
                         "drivers/xrizer/scripts/linux/package_xrizer_compliance.sh",
                         "devices/common/linux/scripts/build/dependency_policy.sh"):
            destination = self.root / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(ROOT / relative, destination)
        tools = self.root / "tools bin"
        tools.mkdir()

        def executable(name, text):
            path = tools / name
            path.write_text("#!/usr/bin/env bash\nset -euo pipefail\n" + text)
            path.chmod(0o755)

        clang = self.root / "libclang"
        clang.mkdir()
        (clang / "libclang.so").touch()
        sysroot = self.root / "rust toolchain"
        rust_src = sysroot / "lib/rustlib/src/rust/library/Cargo.toml"
        rust_src.parent.mkdir(parents=True)
        rust_src.touch()
        for name in ("gcc", "clang", "glslc", "pkg-config"):
            executable(name, "exit 0\n")
        executable("rustup", "echo 'UNEXPECTED_TOOLCHAIN_DOWNLOAD' >&2; exit 91\n")
        executable("rustc", '[[ "$PWD" == "$TEST_XRIZER_SOURCE" ]]; printf "%s\\n" "$TEST_SYSROOT"\n')
        executable("cargo", '''[[ "$PWD" == "$TEST_XRIZER_SOURCE" ]]
[[ "$CARGO_NET_OFFLINE" == true && "$RUSTUP_AUTO_INSTALL" == 0 ]]
if [[ "$*" == 'xbuild --version' ]]; then exit 0; fi
if [[ "$1" == xbuild ]]; then
  mkdir -p target/release; echo fixture > target/release/libopenvr_api.so
elif [[ "$1" == vendor ]]; then
  mkdir -p "${@: -1}"; echo 'directory = "vendor"'
else
  exit 92
fi
''')
        self.env.update(PATH=str(tools) + os.pathsep + self.env['PATH'], LIBCLANG_PATH=str(clang),
                        TEST_SYSROOT=str(sysroot), TEST_XRIZER_SOURCE=str(source),
                        XRIZER_REF=commit, XRIZER_INSTALL_RUST_SRC="1")
        script = ROOT / "drivers/xrizer/scripts/linux/install_xrizer.sh"
        result = self.run_session(["bash", str(script)], XR_OFFLINE="1")
        self.assertEqual(result.returncode, 0, result.stderr)
        installed = self.root / "bin/drivers/xrizer"
        self.assertTrue((installed / "runtime/libopenvr_api.so").is_file())
        archives = list((installed / "source").glob("*.tar.gz"))
        self.assertEqual(len(archives), 1)
        with tarfile.open(archives[0]) as archive:
            self.assertTrue(any(name.endswith("xr-gate-integration/dependency_policy.sh") for name in archive.getnames()))


if __name__ == "__main__":
    unittest.main()
