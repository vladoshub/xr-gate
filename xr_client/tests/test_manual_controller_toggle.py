"""The menu owns stdin and key 5 must toggle the real child without Enter."""
import os
import sys
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from xr_client.config_reader import ServiceSpec
from xr_client.manual_controls import ManualControlPoller
from xr_client.xr_backend_client import Launcher


@unittest.skipUnless(os.name == "posix", "POSIX terminal controls")
class ManualControllerToggleTest(unittest.TestCase):
    def terminal(self):
        import pty
        import termios
        master, slave = pty.openpty()
        source = os.fdopen(slave, "r")
        state = termios.tcgetattr(slave)
        self.addCleanup(os.close, master)
        self.addCleanup(source.close)
        stdin = patch("sys.stdin", source)
        stdin.start()
        self.addCleanup(stdin.stop)
        controls = ManualControlPoller()
        self.addCleanup(controls.close)
        self.assertIsNotNone(controls._posix_fd)
        return master, source, controls, state

    def test_single_key_and_following_enter_only_toggle_once(self):
        master, source, controls, state = self.terminal()
        os.write(master, b"5")
        events = controls.poll_events()
        self.assertEqual([e.name for e in events], ["manual-toggle-override-controller"])
        os.write(master, b"\n")
        self.assertEqual(controls.poll_events(), [])
        controls.close()
        import termios
        self.assertEqual(termios.tcgetattr(source.fileno()), state)

    def test_text_alias_and_backspace_keep_working(self):
        master, _, controls, _ = self.terminal()
        os.write(master, b"controllerr\x7f\n")
        self.assertEqual([e.name for e in controls.poll_events()], ["manual-toggle-override-controller"])

    def test_recovery_prompt_temporarily_restores_line_mode(self):
        import termios
        _, source, controls, state = self.terminal()

        def answer(prompt):
            self.assertEqual(termios.tcgetattr(source.fileno()), state)
            return "r"

        with patch("builtins.input", side_effect=answer):
            self.assertEqual(controls.read_line("Retry? "), "r")
        self.assertFalse(termios.tcgetattr(source.fileno())[3] & termios.ICANON)

    def test_key_five_starts_stops_and_restarts_child(self):
        master, source, controls, state = self.terminal()
        with tempfile.TemporaryDirectory() as folder:
            marker = Path(folder) / "child-stdin"
            script = ("import sys,time; from pathlib import Path; "
                      "Path(sys.argv[1]).write_text(repr(sys.stdin.read(1))); time.sleep(60)")
            spec = ServiceSpec("override_controller", [sys.executable, "-c", script, str(marker)],
                               start_delay_s=0.1, stop_timeout_s=0.2)
            action = {"type": "toggle_service", "service": "override_controller", "wait_streams": False}
            cfg = SimpleNamespace(root_project=folder, log_dir=folder, log_max_bytes=0,
                                  pre_gate_services=[], post_gate_services=[spec], foreground_services=[],
                                  imu_tap_controls=SimpleNamespace(actions={"manual-toggle-override-controller": action}))
            launcher = Launcher(cfg)
            launcher.manual_controls = controls
            try:
                os.write(master, b"5")
                launcher.drain_control_events()
                self.assertTrue(launcher.is_service_running("override_controller"))
                first = launcher.running[0].process
                self.assertEqual(marker.read_text(), "''", "background child inherited the menu's stdin")

                os.write(master, b"5")
                launcher.drain_control_events()
                self.assertIsNotNone(first.poll())
                self.assertFalse(launcher.is_service_running("override_controller"))
                self.assertTrue(launcher._controller_imu_console_suppressed)
                self.assertEqual(launcher.running, [])

                os.write(master, b"5")
                launcher.drain_control_events()
                self.assertTrue(launcher.is_service_running("override_controller"))
                self.assertNotEqual(launcher.running[0].process.pid, first.pid)
                self.assertFalse(launcher._controller_imu_console_suppressed)
            finally:
                launcher.stop_all()
            import termios
            self.assertEqual(termios.tcgetattr(source.fileno()), state)


if __name__ == "__main__":
    unittest.main()
