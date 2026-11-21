"""The device connection owns the entire launcher session, including startup."""
import json
import os
from pathlib import Path
import shlex
import socket
import subprocess
import sys
import tempfile
import time
import unittest

XR_CLIENT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(sys.platform.startswith("linux"), "Linux package server")
class ConfigServerTest(unittest.TestCase):
    def test_disconnect_during_startup_stops_children_and_allows_next_device(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            runner = root / "run_xr_client.sh"
            launcher = XR_CLIENT / "xr_backend_client.py"
            runner.write_text(f"#!/usr/bin/env bash\nexec {shlex.quote(sys.executable)} "
                              f'{shlex.quote(str(launcher))} "$@"\n')
            runner.chmod(0o755)
            device_env = root / "device.env"
            device_env.touch()

            def worker(name):
                return [sys.executable, "-c",
                        "import os, sys, time; from pathlib import Path; "
                        "Path(sys.argv[1]).write_text(str(os.getpid())); time.sleep(60)",
                        str(root / name)]

            (root / "test_profile.json").write_text(json.dumps({
                "device_env": str(device_env),
                "log_dir": str(root / "logs"),
                "clean_registries": False,
                "service_startup_rules": {"enabled": False},
                "pre_gate_services": [{"name": "capture_service", "command": worker("capture.pid")}],
                "gate": {"command": worker("gate.pid"), "timeout_s": 60},
                "post_gate_services": [],
                "foreground_services": [],
            }))
            env = os.environ.copy()
            env.update(XR_CLIENT_CONFIG_DIR=directory, XR_ROOT_PROJECT=directory, XR_DEVICE_ENV=str(device_env))
            with socket.socket() as reservation:
                reservation.bind(("127.0.0.1", 0))
                port = reservation.getsockname()[1]
            with (root / "server.log").open("w") as log:
                server = subprocess.Popen([sys.executable, str(XR_CLIENT / "config_server.py"),
                                           str(runner), "--port", str(port)], env=env,
                                          stdout=log, stderr=subprocess.STDOUT)
            try:
                with self.connect(port) as device:
                    device.sendall(b"../test_profile\n")
                    with device.makefile("rb") as reply:
                        self.assertTrue(reply.readline().startswith(b"ERR "))
                with self.connect(port) as device:
                    device.sendall(b"test_")
                    device.sendall(b"profile\n")
                    with device.makefile("rb") as reply:
                        self.assertEqual(reply.readline(), b"OK\n")
                    self.wait_for(lambda: (root / "gate.pid").stat().st_size, root, "gate did not start")
                    pids = [int((root / f"{name}.pid").read_text()) for name in ("capture", "gate")]
                    with self.assertRaises(ConnectionRefusedError):
                        socket.create_connection(("127.0.0.1", port), timeout=1).close()

                with self.connect(port) as device:
                    for pid in pids:
                        self.assertFalse(Path(f"/proc/{pid}").exists())
                    device.sendall(b"test_profile\n")
                    with device.makefile("rb") as reply:
                        self.assertEqual(reply.readline(), b"OK\n")
            finally:
                server.terminate()
                try:
                    server.wait(timeout=6)
                except subprocess.TimeoutExpired:
                    server.kill()
                    server.wait()
                    raise
            self.assertEqual(server.returncode, 0, (root / "server.log").read_text())

    def connect(self, port):
        deadline = time.monotonic() + 4
        while True:
            try:
                return socket.create_connection(("127.0.0.1", port), timeout=1)
            except ConnectionRefusedError:
                if time.monotonic() >= deadline:
                    raise
                time.sleep(0.02)

    def wait_for(self, predicate, root, message):
        deadline = time.monotonic() + 4
        while time.monotonic() < deadline:
            try:
                if predicate():
                    return
            except FileNotFoundError:
                pass
            time.sleep(0.02)
        self.fail(f"{message}\n{(root / 'server.log').read_text()}")


if __name__ == "__main__":
    unittest.main()
