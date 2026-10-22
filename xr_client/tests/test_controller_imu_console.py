"""Console forwarding must retain file logging and filter verbose frames."""
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
from xr_client.process_manager import RunningProcess
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


def raw_status_logger(messages):
    """Capture backend lines independently of colored user guidance."""
    def collect(message, *, color=None):
        if color is None:
            messages.append(message)
    return collect


class ControllerImuConsoleTest(unittest.TestCase):
    def test_gyro_zeroing_guidance_waits_for_valid_calibrated_orientation(self):
        def frame(left, right):
            return (f"[xr_runtime_adapter] frame=90 controller_left_connected=yes "
                    f"controller_right_connected=yes controller_left_imu_data_flags={left} "
                    f"controller_right_imu_data_flags={right}\n").encode("ascii")
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "xr_runtime_adapter.log"
            progress = b"[xr_runtime_adapter][imu-calibration] side=left yaw=collecting translation=need_motion\n"
            path.write_bytes(progress + frame(327, 327) + progress + frame(359, 359) + frame(367, 327) +
                             frame(367, 367) * 2 +
                             progress +
                             b"[xr_runtime_adapter][WARN] controller IMU state reset: reason=transport_discontinuity; yaw=reset\n" +
                             frame(327, 327) + frame(367, 367))
            item = SimpleNamespace(log_path=path, limiter_stop=threading.Event(),
                                   process=SimpleNamespace(poll=lambda: 0))
            messages = []
            launcher = self.launcher(folder)
            launcher._controller_gyro_zero_pending = True
            with patch("xr_client.xr_backend_client.log", lambda message, **kw: messages.append(message)):
                launcher.forward_controller_imu_status(item)
            ready = "Both controller gyroscopes zeroed. Take the controllers in your hands and continue optical calibration."
            self.assertEqual(messages.count(ready), 2)
            self.assertEqual(sum(message.startswith("Left controller: keep it still") for message in messages), 2)
            self.assertEqual(sum(message.startswith("Right controller: keep it still") for message in messages), 2)
            self.assertEqual(messages.count("Left controller gyroscope zeroed."), 2)
            self.assertEqual(messages.count("Right controller gyroscope zeroed."), 2)
            self.assertEqual(sum(message.startswith("Calibrating left") for message in messages), 1)

    def launcher(self, folder):
        return Launcher(SimpleNamespace(log_dir=folder, root_project=folder, log_max_bytes=0))

    def test_log_limiter_does_not_replay_completed_calibration(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "xr_runtime_adapter.log"
            progress = (b"[xr_runtime_adapter][imu-calibration] side=left yaw=collecting "
                        b"translation=need_motion\n")
            initial = status("no", "no", ("no", "no"))
            ready = status("yes", "yes", ("yes", "yes"))
            history = initial + progress + ready
            path.write_bytes(b"old verbose frames\n" * 400 + history)
            item = RunningProcess(ServiceSpec(name="xr_runtime_adapter", command=[]),
                                  SimpleNamespace(poll=lambda: None), path, None)
            launcher = Launcher(SimpleNamespace(log_dir=folder, root_project=folder,
                                                log_max_bytes=len(history) + 1024,
                                                log_trim_interval_s=0.01))
            launcher.manual_controls = SimpleNamespace(stop_requested=False,
                                                       redraw_prompt=lambda: None)
            ready_seen, replayed = threading.Event(), threading.Event()
            messages = []

            def collect(message, *, color=None):
                messages.append((message, color))
                if message == "Both controllers calibrated. IMU prediction is active.":
                    if ready_seen.is_set():
                        replayed.set()
                    ready_seen.set()

            reader = threading.Thread(target=launcher.forward_controller_imu_status,
                                      args=(item,), daemon=True)
            with patch("xr_client.xr_backend_client.log", collect):
                reader.start()
                try:
                    self.assertTrue(ready_seen.wait(2), "initial calibration missing")
                    launcher.start_log_limiter(item)
                    self.assertFalse(replayed.wait(0.4), "log trimming replayed old calibration")
                    self.assertLessEqual(path.stat().st_size, launcher.cfg.log_max_bytes)
                    # A real calibration reset and later confirmation still reach the user.
                    ready_seen.clear()
                    with path.open("ab") as output:
                        output.write(initial + progress + ready)
                    self.assertTrue(ready_seen.wait(2), "real recalibration was suppressed")
                    self.assertEqual(sum(message.startswith("Calibrating left")
                                         for message, _ in messages), 2)
                finally:
                    item.limiter_stop.set()
                    reader.join(2)
            self.assertFalse(reader.is_alive())

    def test_confirmed_side_ignores_late_progress_until_alignment_resets(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "xr_runtime_adapter.log"
            ready = status("yes", "yes", ("yes", "yes"))
            progress = (b"[xr_runtime_adapter][imu-calibration] side=left yaw=ready "
                        b"translation=need_motion\n")
            reset = status("yes", "yes", ("no", "yes"))
            path.write_bytes(ready + progress + reset + progress + ready + progress)
            item = SimpleNamespace(log_path=path, limiter_stop=threading.Event(),
                                   process=SimpleNamespace(poll=lambda: 0))
            messages = []
            with patch("xr_client.xr_backend_client.log", lambda message, **kw: messages.append(message)):
                self.launcher(folder).forward_controller_imu_status(item)
            self.assertEqual(messages.count(progress.decode().strip()), 1)
            self.assertEqual(sum(message.startswith("Calibrating left") for message in messages), 1)
            self.assertEqual(messages[-1], "Both controllers calibrated. IMU prediction is active.")

    def test_trim_preserves_partial_line_when_log_regrows_past_reader_offset(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "xr_runtime_adapter.log"
            initial = status("no", "no", ("no", "no"))
            ready = status("yes", "yes", ("yes", "yes"))
            path.write_bytes(b"verbose frame\n" * 400 + initial + ready[:-10])
            old_size = path.stat().st_size
            item = RunningProcess(ServiceSpec(name="xr_runtime_adapter", command=[]),
                                  SimpleNamespace(poll=lambda: None), path, None)
            launcher = Launcher(SimpleNamespace(log_dir=folder, root_project=folder,
                                                log_max_bytes=len(ready) + 256))
            initial_seen, resume, ready_seen = threading.Event(), threading.Event(), threading.Event()
            messages = []

            def collect(message, *, color=None):
                if color is not None:
                    return
                messages.append(message)
                if message == initial.decode().strip():
                    initial_seen.set()
                    resume.wait(2)
                if message == ready.decode().strip():
                    ready_seen.set()

            reader = threading.Thread(target=launcher.forward_controller_imu_status,
                                      args=(item,), daemon=True)
            with patch("xr_client.xr_backend_client.log", collect):
                reader.start()
                try:
                    self.assertTrue(initial_seen.wait(2))
                    launcher._trim_service_log(item)
                    with path.open("ab") as output:
                        output.write(ready[-10:] + b"new verbose frame\n" * 500)
                    self.assertGreater(path.stat().st_size, old_size)
                    resume.set()
                    self.assertTrue(ready_seen.wait(2), "new status lost across trimming/regrowth")
                    self.assertEqual(messages, [initial.decode().strip(), ready.decode().strip()])
                finally:
                    resume.set()
                    item.limiter_stop.set()
                    reader.join(2)
            self.assertFalse(reader.is_alive())

    def test_explicit_stop_suppresses_calibration_until_controller_starts_again(self):
        with tempfile.TemporaryDirectory() as folder:
            ready = status("yes", "yes", ("yes", "yes"))
            progress = (b"[xr_runtime_adapter][imu-calibration] side=left yaw=collecting "
                        b"translation=need_motion\n")
            path = Path(folder) / "xr_runtime_adapter.log"
            path.write_bytes(progress + ready)
            item = SimpleNamespace(log_path=path, limiter_stop=threading.Event(),
                                   process=SimpleNamespace(poll=lambda: 0))
            launcher = self.launcher(folder)
            messages = []
            with patch("xr_client.xr_backend_client.log", raw_status_logger(messages)):
                launcher._controller_imu_console_suppressed = True
                launcher.forward_controller_imu_status(item)
                self.assertEqual(messages, [])
                launcher._controller_imu_console_suppressed = False
                launcher.forward_controller_imu_status(item)
            self.assertEqual(messages, [line.decode().strip() for line in (progress, ready)])

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
            with patch("xr_client.xr_backend_client.log", raw_status_logger(messages)):
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
            with patch("xr_client.xr_backend_client.log", raw_status_logger(messages)):
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
            with patch("xr_client.xr_backend_client.log", raw_status_logger(messages)):
                self.launcher(folder).forward_controller_imu_status(item)
            self.assertEqual(messages, [status(a, b).decode().strip()
                                        for a, b in (("no", "no"), ("yes", "no"), ("yes", "yes"))])

    def test_partial_lines_and_log_truncation(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "xr_runtime_adapter.log"
            path.write_bytes(status("no", "no") + b"noise\n" * 1000)
            received = threading.Event()
            messages = []

            def collect(message, *, color=None):
                if color is not None:
                    return
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

            def collect(message, *, color=None):
                if color is not None:
                    return
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
