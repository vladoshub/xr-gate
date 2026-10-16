"""A dead provider cannot keep its Gear VR links alive across stop/restart."""
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
from xr_client.process_manager import cleanup_ble_connections
from xr_client.xr_backend_client import Launcher
import xr_client.xr_backend_client as client

LEFT = "/org/bluez/hci2/dev_AA_BB_CC_DD_EE_01"
RIGHT = "/org/bluez/hci0/dev_AA_BB_CC_DD_EE_02"


class GearVrCleanupTest(unittest.TestCase):
    def item(self, folder, records, *, alive=False):
        path = Path(folder) / "devices.cleanup"
        path.write_text(records)
        return SimpleNamespace(ble_cleanup_file=path,
                               process=SimpleNamespace(poll=lambda: None if alive else -9))

    def test_only_pending_valid_paths_are_disconnected_once(self):
        with tempfile.TemporaryDirectory() as folder:
            item = self.item(folder, f"+{LEFT}\n-{LEFT}\n+{RIGHT}\n+{RIGHT}\n"
                             "+/org/bluez/hci0\n+/org/bluez/hci0/dev_../bad\nnoise\n")
            path = item.ble_cleanup_file
            messages = []
            with patch("xr_client.process_manager.subprocess.run",
                       return_value=SimpleNamespace(returncode=0, stderr="")) as disconnect:
                cleanup_ble_connections(item, log_fn=messages.append)
                cleanup_ble_connections(item, log_fn=messages.append)
            self.assertEqual(disconnect.call_count, 1)
            self.assertEqual(disconnect.call_args.args[0],
                             ["busctl", "--system", "--timeout=1s", "call", "org.bluez",
                              RIGHT, "org.bluez.Device1", "Disconnect"])
            self.assertFalse(path.exists())
            self.assertIsNone(item.ble_cleanup_file)

    def test_live_provider_is_left_alone(self):
        with tempfile.TemporaryDirectory() as folder:
            item = self.item(folder, f"+{LEFT}\n", alive=True)
            with patch("xr_client.process_manager.subprocess.run") as disconnect:
                cleanup_ble_connections(item, log_fn=lambda _: None)
            disconnect.assert_not_called()
            self.assertTrue(item.ble_cleanup_file.exists())

    def test_failed_cleanup_is_bounded_and_reported(self):
        for failure in (FileNotFoundError("busctl unavailable"),
                        subprocess.TimeoutExpired("busctl", 1.5)):
            with self.subTest(failure=failure), tempfile.TemporaryDirectory() as folder:
                item = self.item(folder, f"+{LEFT}\n")
                messages = []
                with patch("xr_client.process_manager.subprocess.run", side_effect=failure):
                    cleanup_ble_connections(item, log_fn=messages.append)
                self.assertTrue(any("[WARN]" in line for line in messages))
                self.assertIsNone(item.ble_cleanup_file)

    @unittest.skipUnless(hasattr(signal, "SIGHUP"), "POSIX stop signals")
    def test_launcher_stop_signals_run_cleanup_and_restore_handlers(self):
        for stop_signal in (signal.SIGTERM, signal.SIGHUP):
            with self.subTest(signal=stop_signal), tempfile.TemporaryDirectory() as folder:
                cfg = SimpleNamespace(log_dir=folder, pre_gate_services=[], prestart_control=None)
                previous = {sig: signal.getsignal(sig) for sig in (signal.SIGTERM, signal.SIGHUP)}

                def receive_signal(*args, **kwargs):
                    handler = signal.getsignal(stop_signal)
                    self.assertTrue(callable(handler))
                    handler(stop_signal, None)

                with patch.object(client, "resolve_config_argument", return_value="fixture"), \
                     patch.object(client, "load_config", return_value=cfg), \
                     patch.object(Launcher, "run", side_effect=receive_signal), \
                     patch.object(Launcher, "stop_all") as cleanup:
                    with self.assertRaises(SystemExit) as stopped:
                        client.main(["--config", "fixture"])
                    self.assertEqual(stopped.exception.code, 128 + stop_signal)
                    cleanup.assert_called_once()
                for sig, handler in previous.items():
                    self.assertEqual(signal.getsignal(sig), handler)

    def start_provider(self, folder):
        launcher = Launcher(SimpleNamespace(log_dir=folder, root_project=folder, log_max_bytes=0))
        script = (
            "import os,signal,time; from pathlib import Path; "
            "signal.signal(signal.SIGTERM, signal.SIG_IGN); "
            f"Path(os.environ['OVERRIDE_CONTROLLER_BLE_CLEANUP_FILE']).write_text('+{LEFT}\\n'); "
            "time.sleep(60)"
        )
        spec = ServiceSpec(name="override_controller", command=[sys.executable, "-c", script],
                           start_delay_s=0.0, stop_timeout_s=0.2, restart_on_exit=True,
                           restart_max_attempts=1, restart_backoff_s=0.0,
                           restart_wait_streams=False)
        launcher.start_service(spec)
        item = launcher.running[0]
        deadline = time.monotonic() + 2
        while not item.ble_cleanup_file.read_text() and time.monotonic() < deadline:
            time.sleep(0.01)
        self.assertEqual(item.ble_cleanup_file.read_text(), f"+{LEFT}\n")
        return launcher, item

    @unittest.skipUnless(sys.platform.startswith("linux"), "Linux process groups / BlueZ")
    def test_forced_manual_stop_disconnects_after_process_death(self):
        with tempfile.TemporaryDirectory() as folder, patch(
                "xr_client.process_manager.subprocess.run",
                return_value=SimpleNamespace(returncode=0, stderr="")) as disconnect:
            launcher, item = self.start_provider(folder)
            cleanup_path = item.ble_cleanup_file
            try:
                launcher.stop_service_names(["override_controller"])
                self.assertEqual(item.process.returncode, -signal.SIGKILL)
                self.assertEqual(launcher.running, [])
                self.assertFalse(cleanup_path.exists())
                disconnect.assert_called_once()
            finally:
                launcher.stop_all()

    @unittest.skipUnless(sys.platform.startswith("linux"), "Linux process groups / BlueZ")
    def test_sigkill_is_cleaned_before_restart(self):
        with tempfile.TemporaryDirectory() as folder, patch(
                "xr_client.process_manager.subprocess.run",
                return_value=SimpleNamespace(returncode=0, stderr="")) as disconnect:
            launcher, item = self.start_provider(folder)
            cleanup_path = item.ble_cleanup_file
            try:
                os.kill(item.process.pid, signal.SIGKILL)
                item.process.wait(timeout=2)

                def restart(spec):
                    disconnect.assert_called_once()
                    self.assertFalse(cleanup_path.exists())
                    self.assertNotIn(item, launcher.running)
                    return True

                with patch.object(launcher, "start_service", side_effect=restart):
                    self.assertTrue(launcher.handle_restartable_exit(item, -signal.SIGKILL))
            finally:
                launcher.stop_all()


if __name__ == "__main__":
    unittest.main()
