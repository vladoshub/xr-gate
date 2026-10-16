"""Startup must recover from USB re-enumeration without accepting stale SHM."""
import json
import os
import sys
import tempfile
import threading
import time
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from xr_client.config_reader import GateSpec, ServiceSpec, WaitStream, parse_service
from xr_client.process_manager import terminate_one_process_tree
from xr_client.xr_backend_client import Launcher


class StartupCameraRecoveryTest(unittest.TestCase):
    def launcher(self, folder, *, attempts=3):
        gate = GateSpec(enabled=True, command=[sys.executable, "-c", "pass"],
                        max_attempts=attempts, restart_backoff_s=0,
                        restart_services_on_failure=["capture_service"],
                        restart_clean_registries=[str(Path(folder) / "capture.json")])
        return Launcher(SimpleNamespace(
            root_project=folder, log_dir=folder, log_max_bytes=0,
            log_trim_interval_s=2, clean_registries=False, prestart_control=None,
            gate=gate, pre_gate_services=[], post_gate_services=[],
            foreground_services=[], imu_tap_controls=None))

    def test_dead_producer_cannot_be_ready_from_its_registry(self):
        with tempfile.TemporaryDirectory() as folder:
            launcher = self.launcher(folder)
            launcher.running = [SimpleNamespace(
                spec=ServiceSpec("capture_service", []),
                process=SimpleNamespace(poll=lambda: 19), log_path=Path(folder) / "capture.log")]
            with patch("xr_client.xr_backend_client.registry_has_stream", return_value=True):
                with self.assertRaisesRegex(RuntimeError, "stopped before stream"):
                    launcher.wait_stream("capture_service", WaitStream("stale.json", "camera0"))

    def test_first_device_open_failure_recovers_in_full_startup(self):
        with tempfile.TemporaryDirectory() as folder:
            launcher = self.launcher(folder)
            marker = Path(folder) / "attempt"
            script = (
                "import sys,time; from pathlib import Path; p=Path(sys.argv[1]); "
                "n=int(p.read_text())+1 if p.exists() else 1; p.write_text(str(n)); "
                "sys.exit(19) if n==1 else time.sleep(60)"
            )
            spec = ServiceSpec("capture_service", [sys.executable, "-c", script, str(marker)],
                               start_delay_s=0.1)
            launcher.cfg.pre_gate_services = [spec]
            previous_cwd = Path.cwd()
            try:
                launcher.run(once=True)
                self.assertEqual(marker.read_text(), "2")
                self.assertTrue(launcher.is_running("capture_service"))
                self.assertEqual(len(launcher.running), 1)
            finally:
                launcher.stop_all()
                os.chdir(previous_cwd)

    def test_recovery_is_bounded_when_device_never_returns(self):
        with tempfile.TemporaryDirectory() as folder:
            launcher = self.launcher(folder, attempts=2)
            spec = ServiceSpec("capture_service", [sys.executable, "-c", "raise SystemExit(19)"],
                               start_delay_s=0.1)
            with patch.object(launcher, "start_service", wraps=launcher.start_service) as start:
                with self.assertRaisesRegex(RuntimeError, "exited immediately"):
                    launcher.start_service_with_gate_recovery(spec)
            self.assertEqual(start.call_count, 2)
            self.assertEqual(launcher.running, [])

    def test_gate_retry_also_recovers_transient_start_failures(self):
        with tempfile.TemporaryDirectory() as folder:
            launcher = self.launcher(folder)
            spec = ServiceSpec("capture_service", [sys.executable])
            launcher.cfg.pre_gate_services = [spec]
            registry = Path(folder) / "capture.json"
            registry.write_text("stale registry")
            with patch.object(launcher, "start_service", side_effect=[RuntimeError("device missing"), True]) as start, \
                 patch.object(launcher, "wait_for_service_streams") as wait:
                launcher._restart_services_for_gate_retry(launcher.cfg.gate)
            self.assertEqual(start.call_count, 2)
            wait.assert_called_once_with(spec)
            self.assertFalse(registry.exists())

    def test_gate_does_not_wait_for_timeout_after_capture_exits(self):
        with tempfile.TemporaryDirectory() as folder:
            launcher = self.launcher(folder)
            gate = launcher.cfg.gate
            gate.command = [sys.executable, "-c", "import time; time.sleep(60)"]
            gate.timeout_s = 60
            launcher.running = [SimpleNamespace(
                spec=ServiceSpec("capture_service", []),
                process=SimpleNamespace(poll=lambda: 19, returncode=19))]

            def reconnect(_gate):
                launcher.running = []
                gate.command = [sys.executable, "-c", "print('[xr_startup_gate] startup gate passed: healthy')"]

            def reap(process):
                terminate_one_process_tree(process)
                process.stdout.close()

            with patch.object(launcher, "_restart_services_for_gate_retry", side_effect=reconnect) as restart, \
                 patch.object(launcher, "_release_startup_gate_process", side_effect=reap), \
                 patch.object(launcher, "_prompt_after_gate_failure", side_effect=AssertionError("unexpected prompt")):
                started = time.monotonic()
                launcher.run_gate(gate)
                self.assertLess(time.monotonic() - started, 3)
            restart.assert_called_once()

    def test_vo_and_no_imu_do_not_require_imu_activity(self):
        for vo, no_imu in ((True, False), (False, True)):
            with self.subTest(vo=vo, no_imu=no_imu), tempfile.TemporaryDirectory() as folder:
                launcher = self.launcher(folder)
                if vo:
                    launcher.cfg.post_gate_services = [ServiceSpec("basalt_vio", ["basalt", "--mode", "vo"])]
                spec = ServiceSpec("capture_service", [], no_imu=no_imu,
                                   wait_streams=[WaitStream("registry", s) for s in ("camera0", "camera1", "imu0")])
                with patch.object(launcher, "wait_stream"), \
                     patch.object(launcher, "wait_for_stream_activity") as activity:
                    launcher.wait_for_service_streams(spec)
                self.assertEqual([w.stream for w in activity.call_args.args[1]], ["camera0", "camera1"])

    @unittest.skipUnless(sys.platform.startswith("linux"), "POSIX SHM readiness")
    def test_shm_readiness_requires_new_samples_from_both_cameras(self):
        from capture_client.shm_ring import ShmRingPublisher
        from capture_client.stream_defs import FormatCode, StreamKind, StreamSpec

        with tempfile.TemporaryDirectory() as folder:
            launcher = self.launcher(folder)
            launcher.cfg.root_project = str(Path(__file__).resolve().parents[2])
            publishers = [ShmRingPublisher(StreamSpec(
                stream, StreamKind.IMAGE, width=2, height=2,
                format_code=int(FormatCode.GRAY8), payload_size=4), namespace=Path(folder).name)
                for stream in ("camera0", "camera1")]
            registry = Path(folder) / "capture.json"
            registry.write_text(json.dumps({"streams": {p.spec.stream_id: p.registry_entry() for p in publishers}}))
            spec = parse_service({"name": "capture_service", "command": ["capture"],
                                  "stream_activity_timeout_s": 0.15,
                                  "stream_activity_stable_s": 0}, folder, 30)
            waits = [WaitStream(str(registry), p.spec.stream_id) for p in publishers]
            launcher.running = [SimpleNamespace(spec=spec, process=SimpleNamespace(poll=lambda: None))]
            try:
                for publisher in publishers:
                    publisher.publish(b"1234")
                # Cached frames are present, but do not prove a working device.
                with self.assertRaisesRegex(RuntimeError, "no fresh data.*camera0.*camera1"):
                    launcher.wait_for_stream_activity(spec, waits)

                real_sleep = time.sleep

                def publish_left(_):
                    publishers[0].publish(b"1234")
                    real_sleep(0.01)

                with patch("xr_client.xr_backend_client.time.sleep", side_effect=publish_left):
                    with self.assertRaisesRegex(RuntimeError, "no fresh data on streams: camera1$"):
                        launcher.wait_for_stream_activity(spec, waits)

                def publish_after_attach():
                    time.sleep(0.03)
                    for publisher in publishers:
                        publisher.publish(b"5678")

                thread = threading.Thread(target=publish_after_attach)
                thread.start()
                try:
                    launcher.wait_for_stream_activity(spec, waits)
                finally:
                    thread.join(1)
            finally:
                launcher.running = []
                for publisher in publishers:
                    publisher.close()
                    # The SDK unregisters manually owned SHM from resource_tracker.
                    (Path("/dev/shm") / publisher.shm_name).unlink(missing_ok=True)


if __name__ == "__main__":
    unittest.main()
