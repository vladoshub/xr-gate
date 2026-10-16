"""A dead launcher shell must not leave a publishing child alive."""
import os
import signal
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from xr_client.config_reader import ServiceSpec
from xr_client.process_manager import terminate_one_process_tree
from xr_client.xr_backend_client import Launcher


@unittest.skipUnless(sys.platform.startswith("linux"), "Linux process-group lifetime")
class ProcessGroupShutdownTest(unittest.TestCase):
    def publisher(self, folder, *, wrapper_exits=False):
        pid_file = Path(folder) / "publisher.pid"
        child = ("import os,signal,sys,time; from pathlib import Path; "
                 "signal.signal(signal.SIGTERM, signal.SIG_IGN); "
                 "Path(sys.argv[1]).write_text(str(os.getpid())); time.sleep(60)")
        wrapper = ("import subprocess,sys,time; "
                   "subprocess.Popen([sys.executable,'-c',sys.argv[1],sys.argv[2]]); "
                   + ("time.sleep(0.05)" if wrapper_exits else "time.sleep(60)"))
        proc = subprocess.Popen([sys.executable, "-c", wrapper, child, str(pid_file)],
                                stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                                stderr=subprocess.DEVNULL, start_new_session=True)
        self.addCleanup(self.cleanup_group, proc)
        deadline = time.monotonic() + 2
        while not pid_file.exists() and time.monotonic() < deadline:
            time.sleep(0.01)
        self.assertTrue(pid_file.exists(), "child did not start")
        return proc, int(pid_file.read_text())

    @staticmethod
    def cleanup_group(proc):
        try:
            os.killpg(proc.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        proc.wait(timeout=2)

    def assert_child_stopped(self, pid):
        path = Path(f"/proc/{pid}/stat")
        try:
            state = path.read_text().rsplit(")", 1)[1].split()[0]
        except FileNotFoundError:
            return
        self.assertIn(state, ("Z", "X"), "child still running after wrapper exited")

    def test_manual_stop_kills_child_after_wrapper_dies_on_sigterm(self):
        with tempfile.TemporaryDirectory() as folder:
            proc, child_pid = self.publisher(folder)
            spec = ServiceSpec("override_controller", [], stop_timeout_s=0.2)
            launcher = Launcher(SimpleNamespace(log_dir=folder))
            launcher.running = [SimpleNamespace(spec=spec, process=proc,
                                               limiter_stop=None, ble_cleanup_file=None,
                                               log_file=open(os.devnull, "w"))]
            launcher.stop_service_names(["override_controller"])
            self.assertIsNotNone(proc.poll())
            self.assert_child_stopped(child_pid)
            self.assertEqual(launcher.running, [])

    def test_one_shot_shutdown_still_kills_children_when_wrapper_already_exited(self):
        with tempfile.TemporaryDirectory() as folder:
            proc, child_pid = self.publisher(folder, wrapper_exits=True)
            proc.wait(timeout=2)
            terminate_one_process_tree(proc, graceful_wait_s=0.1)
            self.assert_child_stopped(child_pid)

    def test_failed_stop_is_reported_and_service_is_not_forgotten(self):
        launcher = Launcher(SimpleNamespace(log_dir="/tmp"))
        item = SimpleNamespace(spec=ServiceSpec("override_controller", []))
        launcher.running = [item]
        with patch("xr_client.xr_backend_client.terminate_running_processes", return_value=False):
            with self.assertRaisesRegex(RuntimeError, "Services did not stop"):
                launcher.stop_service_names(["override_controller"])
        self.assertEqual(launcher.running, [item])
        self.assertFalse(launcher._controller_imu_console_suppressed)


if __name__ == "__main__":
    unittest.main()
