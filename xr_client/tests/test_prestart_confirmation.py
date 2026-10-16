"""Capture must wait for an actual XREAL mode readback, with unbuffered logs."""
import shutil
import subprocess
import sys
import tempfile
import textwrap
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from xr_client.config_reader import PrestartControlSpec, PrestartOptionSpec
from xr_client.xr_backend_client import Launcher


class PrestartConfirmationTest(unittest.TestCase):
    def test_alive_fallback_and_setting_message_do_not_confirm_xreal_mode(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "helper.log"
            path.write_text("[xreal_display_helper] setting display mode: 9 (high-refresh-sbs 90Hz)\n")
            clock = SimpleNamespace(now=0.0)
            launcher = Launcher(SimpleNamespace(log_dir=folder))

            def advance(_):
                clock.now += 0.1
                if clock.now >= 0.4:
                    path.write_text("[xreal_display_helper] display mode after:  9 (high-refresh-sbs 90Hz)\n")

            with patch.object(launcher, "is_service_running", return_value=True), \
                 patch("xr_client.xr_backend_client.time.monotonic", side_effect=lambda: clock.now), \
                 patch("xr_client.xr_backend_client.time.sleep", side_effect=advance):
                launcher.wait_prestart_readiness("xreal_display_helper", path,
                                                ["display mode after:  9", "high-refresh-sbs 90Hz"],
                                                1, 0.1, 0)
            self.assertGreaterEqual(clock.now, 0.4)

    def test_empty_log_times_out_instead_of_starting_capture(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "helper.log"
            path.touch()
            clock = SimpleNamespace(now=0.0)
            launcher = Launcher(SimpleNamespace(log_dir=folder))

            def advance(_):
                clock.now += 0.1

            with patch.object(launcher, "is_service_running", return_value=True), \
                 patch("xr_client.xr_backend_client.time.monotonic", side_effect=lambda: clock.now), \
                 patch("xr_client.xr_backend_client.time.sleep", side_effect=advance):
                with self.assertRaisesRegex(RuntimeError, "Timeout waiting.*readiness"):
                    launcher.wait_prestart_readiness("xreal_display_helper", path,
                                                    ["display mode after:  9"], 0.5, 0.1, 0)

    def test_numeric_mode_must_match_instead_of_sharing_a_prefix(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "helper.log"
            path.write_text("[xreal_display_helper] alive; mode=90 (unknown)\n")
            clock = SimpleNamespace(now=0.0)
            launcher = Launcher(SimpleNamespace(log_dir=folder))

            def advance(_):
                clock.now += 0.1

            with patch.object(launcher, "is_service_running", return_value=True), \
                 patch("xr_client.xr_backend_client.time.monotonic", side_effect=lambda: clock.now), \
                 patch("xr_client.xr_backend_client.time.sleep", side_effect=advance):
                with self.assertRaisesRegex(RuntimeError, "Timeout waiting.*readiness"):
                    launcher.wait_prestart_readiness("xreal_display_helper", path,
                                                    ["alive; mode=9"], 0.3, 0.1, 0)

    @unittest.skipUnless(sys.platform.startswith("linux") and shutil.which("c++") and shutil.which("stdbuf"),
                         "Linux C++ helper buffering")
    def test_existing_cpp_stdout_is_visible_through_shell_wrapper(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            source = root / "helper.cpp"
            binary = root / "helper"
            source.write_text(textwrap.dedent('''
                #include <iostream>
                #include <chrono>
                #include <thread>
                int main() {
                  std::cout << "[xreal_display_helper] setting display mode: 9 (high-refresh-sbs 90Hz)\\n";
                  std::this_thread::sleep_for(std::chrono::milliseconds(700));
                  std::cout << "[xreal_display_helper] display mode after:  9 (high-refresh-sbs 90Hz)\\n";
                  std::this_thread::sleep_for(std::chrono::seconds(60));
                }
            '''))
            subprocess.run([shutil.which("c++"), "-std=c++17", str(source), "-o", str(binary)], check=True)
            wrapper = root / "wrapper.sh"
            wrapper.write_text('#!/bin/sh\nexec "$1"\n')
            option = PrestartOptionSpec("90hz", "2", "90Hz", service_name="xreal_display_helper",
                                       command=["/bin/sh", str(wrapper), str(binary)],
                                       wait_log_any=["display mode after:  9", "high-refresh-sbs 90Hz"],
                                       wait_timeout_s=3, readiness_min_alive_s=0.1)
            cfg = SimpleNamespace(log_dir=folder, root_project=folder, log_max_bytes=0,
                                  prestart_control=PrestartControlSpec(enabled=True, prompt=False,
                                                                     selected_option="90hz", options=[option]))
            launcher = Launcher(cfg)
            try:
                launcher.run_prestart_control()
                self.assertTrue(launcher.is_running("xreal_display_helper"))
                self.assertIn("display mode after:  9", (root / "xreal_display_helper.log").read_text())
            finally:
                launcher.stop_all()


if __name__ == "__main__":
    unittest.main()
