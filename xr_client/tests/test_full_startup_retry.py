"""A retry must replace the complete startup cascade, with one shared budget."""
import json
import os
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from xr_client.config_reader import (
    GateSpec, PrestartControlSpec, PrestartOptionSpec, ServiceSpec, parse_gate,
)
from xr_client.xr_backend_client import Launcher


class FullStartupRetryTest(unittest.TestCase):
    def launcher(self, folder, phase="open", attempts=5):
        root = Path(folder)
        # Independent subprocesses record which generation started. Files used
        # as counters are deliberately outside the registries being cleaned.
        record = (
            "import json,os,sys,time; from pathlib import Path; "
            f"root=Path({folder!r}); "
            "kind=sys.argv[1]; marker=root/(kind+'.count'); "
            "n=int(marker.read_text())+1 if marker.exists() else 1; marker.write_text(str(n)); "
            "events=root/'events.jsonl'; "
            "events.open('a').write(json.dumps([kind,n,os.getpid()])+'\\n'); "
        )
        stale = (
            "(root/'capture.json').write_text('stale'); "
            "(root/'tracking.json').write_text('stale'); "
        )
        helper = record + (
            "assert not (root/'capture.json').exists(); "
            "assert not (root/'tracking.json').exists(); "
            + ("sys.exit(19) if n==1 else None; " if phase == "helper" else "")
            + "print('[xreal_display_helper] display mode after:  9',flush=True); time.sleep(60)"
        )
        capture = record + stale + (
            "print('[capture_service_cpp][ERROR] no valid stereo pairs',flush=True); "
            + ("sys.exit(19) if n==1 else None; " if phase == "open" else "")
            + ("sys.exit(19); " if phase == "always" else "")
            + "time.sleep(60)"
        )
        gate = record + (
            ("sys.exit(7) if n==1 else None; " if phase == "gate" else "")
            + ("import signal; signal.signal(signal.SIGTERM,signal.SIG_IGN); "
               "time.sleep(60) if n==1 else None; " if phase == "timeout" else "")
            + "print('[xr_startup_gate] startup gate passed: healthy',flush=True)"
        )
        py = lambda script, name: [sys.executable, "-u", "-c", script, name]
        option = PrestartOptionSpec(
            "90hz", "2", "90Hz", service_name="xreal_display_helper",
            command=py(helper, "helper"), wait_log_any=["display mode after:  9"],
            wait_timeout_s=2,
        )
        current = PrestartOptionSpec("current", "3", "Current", command=[])
        prestart = PrestartControlSpec(enabled=True, prompt=True, selected_option="prompt",
                                       default_option="current", options=[option, current])
        cfg = SimpleNamespace(
            root_project=folder, log_dir=folder, log_max_bytes=0, log_trim_interval_s=2,
            clean_registries=True, registries_to_clean=[str(root/'tracking.json')],
            prestart_control=prestart,
            # A pre-gate dependency must be restarted along with capture/helper.
            pre_gate_services=[ServiceSpec("sidecar", py(record+"time.sleep(60)", "sidecar"), start_delay_s=.05),
                               ServiceSpec("capture_service", py(capture, "capture"), start_delay_s=.1)],
            gate=GateSpec(enabled=True, command=py(gate, "gate"), max_attempts=attempts,
                          timeout_s=.2 if phase == "timeout" else 2, restart_backoff_s=0,
                          restart_services_on_failure=["capture_service"],
                          restart_clean_registries=[str(root/'capture.json')]),
            post_gate_services=[ServiceSpec("backend", py(record+"time.sleep(60)", "backend"), start_delay_s=.05)],
            foreground_services=[], imu_tap_controls=None,
        )
        return Launcher(cfg)

    def events(self, folder):
        return [json.loads(line) for line in (Path(folder)/"events.jsonl").read_text().splitlines()]

    def test_every_startup_failure_replaces_helper_and_pre_gate_services(self):
        for phase in ("helper", "open", "streams", "gate", "timeout"):
            with self.subTest(phase=phase), tempfile.TemporaryDirectory() as folder:
                launcher = self.launcher(folder, phase)
                original_wait = launcher.wait_for_service_streams
                original_start = launcher.start_service
                original_cwd = Path.cwd()

                def wait(spec):
                    if (phase == "streams" and spec.name == "capture_service"
                            and (Path(folder)/"capture.count").read_text() == "1"):
                        raise RuntimeError("no fresh stereo pairs")
                    original_wait(spec)

                def start(spec):
                    if phase == "timeout" and spec.name == "xreal_display_helper" and (Path(folder)/"gate.count").exists():
                        for kind, _, pid in self.events(folder):
                            if kind == "gate":
                                with self.assertRaises(ProcessLookupError, msg="old gate survived into the next attempt"):
                                    os.kill(pid, 0)
                    return original_start(spec)

                try:
                    with patch("builtins.input", return_value="2") as prompt, \
                         patch("xr_client.xr_backend_client.sys.stdin.isatty", return_value=True), \
                         patch.object(launcher, "start_service", side_effect=start), \
                         patch.object(launcher, "wait_for_service_streams", side_effect=wait):
                        launcher.run(once=True)
                    prompt.assert_called_once()
                    events = self.events(folder)
                    helpers = [event for event in events if event[0] == "helper"]
                    self.assertEqual(len(helpers), 2)
                    self.assertNotEqual(helpers[0][2], helpers[1][2])
                    # The first helper was reaped, rather than left alongside its replacement.
                    with self.assertRaises(ProcessLookupError):
                        os.kill(helpers[0][2], 0)
                    self.assertEqual(sum(event[0] == "sidecar" for event in events), 1 if phase == "helper" else 2)
                    self.assertEqual(sum(event[0] == "backend" for event in events), 1)
                    self.assertEqual(sum(event[0] == "gate" for event in events), 2 if phase in ("gate", "timeout") else 1)
                    self.assertFalse(launcher.stop_requested)
                    snapshots = list((Path(folder)/"startup_attempts").glob("*/attempt-1"))
                    self.assertEqual(len(snapshots), 1)
                    self.assertTrue((snapshots[0]/"xreal_display_helper.log").exists())
                    if phase != "helper":
                        self.assertTrue((snapshots[0]/"capture_service.log").exists())
                finally:
                    launcher.stop_all()
                    os.chdir(original_cwd)

    def test_dead_camera_exhausts_one_shared_budget_and_leaves_no_children(self):
        with tempfile.TemporaryDirectory() as folder:
            launcher = self.launcher(folder, "always", attempts=3)
            launcher.cfg.prestart_control.selected_option = "90hz"
            original_cwd = Path.cwd()
            try:
                with self.assertRaisesRegex(RuntimeError, "after 3 full attempt"):
                    launcher.run(once=True)
                events = self.events(folder)
                self.assertEqual(sum(event[0] == "helper" for event in events), 3)
                self.assertEqual(sum(event[0] == "capture" for event in events), 3)
                self.assertFalse(any(event[0] in ("gate", "backend") for event in events))
                self.assertEqual(launcher.running, [])
                self.assertFalse((Path(folder)/"capture.json").exists())
                self.assertFalse((Path(folder)/"tracking.json").exists())
                for _, _, pid in events:
                    with self.assertRaises(ProcessLookupError):
                        os.kill(pid, 0)
            finally:
                launcher.stop_all()
                os.chdir(original_cwd)

    def test_current_mode_stays_current_on_retry(self):
        with tempfile.TemporaryDirectory() as folder:
            launcher = self.launcher(folder)
            launcher.cfg.prestart_control.selected_option = "current"
            original_cwd = Path.cwd()
            try:
                launcher.run(once=True)
                events = self.events(folder)
                self.assertFalse(any(event[0] == "helper" for event in events))
                self.assertEqual(sum(event[0] == "capture" for event in events), 2)
                self.assertEqual(sum(event[0] == "backend" for event in events), 1)
            finally:
                launcher.stop_all()
                os.chdir(original_cwd)

    def test_mode_is_default_for_existing_configs_and_can_be_disabled(self):
        default = parse_gate({"command": [sys.executable]}, ".")
        legacy = parse_gate({"command": [sys.executable], "restart_full_startup_on_failure": False}, ".")
        self.assertTrue(default.restart_full_startup_on_failure)
        self.assertFalse(legacy.restart_full_startup_on_failure)

    def test_manual_gate_retry_also_restarts_the_display_helper(self):
        with tempfile.TemporaryDirectory() as folder:
            launcher = self.launcher(folder, "gate", attempts=1)
            launcher.cfg.prestart_control.selected_option = "90hz"
            original_cwd = Path.cwd()
            try:
                with patch.object(launcher, "_prompt_after_gate_failure", return_value="retry") as prompt:
                    launcher.run(once=True)
                prompt.assert_called_once()
                events = self.events(folder)
                self.assertEqual(sum(event[0] == "helper" for event in events), 2)
                self.assertEqual(sum(event[0] == "gate" for event in events), 2)
                self.assertEqual(sum(event[0] == "backend" for event in events), 1)
            finally:
                launcher.stop_all()
                os.chdir(original_cwd)

    def test_legacy_option_retries_capture_without_restarting_helper(self):
        with tempfile.TemporaryDirectory() as folder:
            launcher = self.launcher(folder)
            launcher.cfg.gate.restart_full_startup_on_failure = False
            launcher.cfg.prestart_control.selected_option = "90hz"
            original_cwd = Path.cwd()
            try:
                launcher.run(once=True)
                events = self.events(folder)
                self.assertEqual(sum(event[0] == "helper" for event in events), 1)
                self.assertEqual(sum(event[0] == "sidecar" for event in events), 1)
                self.assertEqual(sum(event[0] == "capture" for event in events), 2)
            finally:
                launcher.stop_all()
                os.chdir(original_cwd)

    def test_failed_shutdown_does_not_start_another_cascade(self):
        with tempfile.TemporaryDirectory() as folder:
            launcher = self.launcher(folder)
            launcher.cfg.prestart_control.selected_option = "90hz"
            original_cwd = Path.cwd()
            try:
                with patch("xr_client.xr_backend_client.terminate_running_processes", return_value=False):
                    with self.assertRaisesRegex(RuntimeError, "previous service processes did not stop"):
                        launcher.run(once=True)
                self.assertEqual(sum(event[0] == "helper" for event in self.events(folder)), 1)
            finally:
                launcher.stop_all()
                os.chdir(original_cwd)


if __name__ == "__main__":
    unittest.main()
