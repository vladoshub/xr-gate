"""Console forwarding must retain file logging and emit only IMU transitions."""
import subprocess
import sys
import tempfile
import threading
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from xr_client.config_reader import ServiceSpec
from xr_client.xr_backend_client import Launcher


def status(left, right, translation=None):
    text = (
        "[xr_runtime_adapter][imu-alignment] "
        f"controller_left_acceleration_yaw_valid={left} "
        f"controller_right_acceleration_yaw_valid={right}"
    )
    if translation is not None:
        text += (
            f" controller_left_acceleration_translation_yaw_calibrated={translation[0]}"
            f" controller_right_acceleration_translation_yaw_calibrated={translation[1]}"
        )
    return (text + "\n").encode("ascii")


class ControllerImuConsoleTest(unittest.TestCase):
    def launcher(self, folder):
        return Launcher(SimpleNamespace(log_dir=folder, root_project=folder, log_max_bytes=0))

    def test_progress_is_forwarded_even_after_both_yaw_flags_are_ready(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "xr_runtime_adapter.log"
            ready = status("yes", "yes", ("no", "no"))
            progress = (b"[xr_runtime_adapter][imu-calibration] side=left yaw=ready "
                        b"translation=need_motion rotation_stable_ms=0 translation_stable_ms=0 "
                        b"required_stable_ms=4000 support=0 coherence=0 motion_mm=1.2 scale=0\r\n")
            complete = status("yes", "yes", ("yes", "yes"))
            path.write_bytes(ready + progress + ready + progress +
                             b"[xr_runtime_adapter] verbose frame\n" + complete)
            item = SimpleNamespace(log_path=path, limiter_stop=threading.Event(),
                                   process=SimpleNamespace(poll=lambda: 0))
            messages = []
            with patch("xr_client.xr_backend_client.log", messages.append):
                self.launcher(folder).forward_controller_imu_status(item)
            self.assertEqual(messages, [line.decode().strip()
                                       for line in (ready, progress, progress, complete)])

    def test_extended_status_reports_translation_changes_without_repeating_ticks(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "xr_runtime_adapter.log"
            expected = [status("no", "no", ("no", "no")),
                        status("yes", "yes", ("no", "no")),
                        status("yes", "yes", ("yes", "no")),
                        status("yes", "yes", ("yes", "yes"))]
            noise = b"[xr_runtime_adapter] frame=1 controller_left_acceleration_yaw_valid=yes\n"
            path.write_bytes(b"".join(line * 3 + noise for line in expected))
            item = SimpleNamespace(log_path=path, limiter_stop=threading.Event(),
                                   process=SimpleNamespace(poll=lambda: 0))
            messages = []
            with patch("xr_client.xr_backend_client.log", messages.append):
                self.launcher(folder).forward_controller_imu_status(item)
            self.assertEqual(messages, [line.decode().strip() for line in expected])

    def test_exited_service_drains_log_without_forwarding_verbose_frames(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "xr_runtime_adapter.log"
            noise = b"[xr_runtime_adapter] frame=1 controller_left_acceleration_yaw_valid=yes\n"
            path.write_bytes(status("no", "no") + noise * 2000 + status("no", "no")
                             + status("yes", "no") + b"x" * 200000 + b"\n"
                             + status("yes", "yes"))
            item = SimpleNamespace(log_path=path, limiter_stop=threading.Event(),
                                   process=SimpleNamespace(poll=lambda: 0))
            messages = []
            with patch("xr_client.xr_backend_client.log", messages.append):
                self.launcher(folder).forward_controller_imu_status(item)
            self.assertEqual(messages, [status(a, b).decode().strip()
                                        for a, b in (("no", "no"), ("yes", "no"), ("yes", "yes"))])

    def test_partial_lines_and_log_truncation(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "xr_runtime_adapter.log"
            path.write_bytes(status("no", "no") + b"noise\n" * 1000)
            received = threading.Event()
            messages = []

            def collect(message):
                messages.append(message)
                received.set()

            item = SimpleNamespace(log_path=path, limiter_stop=threading.Event(),
                                   process=SimpleNamespace(poll=lambda: None))
            thread = threading.Thread(target=self.launcher(folder).forward_controller_imu_status,
                                      args=(item,), daemon=True)
            with patch("xr_client.xr_backend_client.log", collect):
                thread.start()
                try:
                    self.assertTrue(received.wait(2), "initial status missing")
                    received.clear()
                    # Replace the same inode with a smaller tail, as the limiter does.
                    path.write_bytes(status("yes", "no")[:-1])
                    self.assertFalse(received.wait(0.2), "incomplete line was printed")
                    with path.open("ab") as output:
                        output.write(b"\n")
                    self.assertTrue(received.wait(2), "status lost after log truncation")
                    self.assertEqual(messages[-1], status("yes", "no").decode().strip())
                finally:
                    item.limiter_stop.set()
                    thread.join(2)
            self.assertFalse(thread.is_alive(), "status reader ignored service stop")

    def test_start_service_forwards_to_console_and_preserves_service_log(self):
        with tempfile.TemporaryDirectory() as folder:
            initial = status("no", "no", ("no", "no"))
            ready = status("yes", "yes", ("yes", "yes"))
            script = (
                "import sys,time; "
                f"sys.stdout.buffer.write({initial!r}+{ready!r}); sys.stdout.flush(); "
                "time.sleep(5)"
            )
            spec = ServiceSpec(name="xr_runtime_adapter", command=[sys.executable, "-c", script],
                               start_delay_s=0.1)
            launcher = self.launcher(folder)
            ready_seen = threading.Event()
            messages = []

            def collect(message):
                messages.append(message)
                if message == ready.decode().strip():
                    ready_seen.set()

            with patch("xr_client.xr_backend_client.log", collect):
                try:
                    self.assertTrue(launcher.start_service(spec))
                    self.assertTrue(ready_seen.wait(2), "service start did not forward readiness")
                    self.assertEqual((Path(folder) / "xr_runtime_adapter.log").read_bytes(), initial + ready)
                finally:
                    for item in launcher.running:
                        item.process.terminate()
                        item.process.wait(timeout=2)
                        item.limiter_stop.set()
                        item.log_file.close()


if __name__ == "__main__":
    unittest.main()
