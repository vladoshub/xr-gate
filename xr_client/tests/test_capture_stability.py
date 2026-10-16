"""One initial burst or unrelated camera timestamps cannot start the gate."""
import json
import struct
import sys
import tempfile
import threading
import time
import unittest
from pathlib import Path
from types import SimpleNamespace

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from capture_client.shm_ring import ShmRingPublisher
from capture_client.stream_defs import FormatCode, StreamKind, StreamSpec
from xr_client.config_reader import ServiceSpec, WaitStream
from xr_client.xr_backend_client import Launcher


@unittest.skipUnless(sys.platform.startswith("linux"), "POSIX SHM stability")
class CaptureStabilityTest(unittest.TestCase):
    def check_capture(self, *, skew_ns=0, burst=False, max_delta_ms=1):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            publishers = [ShmRingPublisher(StreamSpec(
                name, StreamKind.IMAGE if name.startswith("camera") else StreamKind.IMU,
                width=2 if name.startswith("camera") else 0,
                height=2 if name.startswith("camera") else 0,
                format_code=int(FormatCode.GRAY8 if name.startswith("camera") else FormatCode.IMU_F32_LE),
                payload_size=4 if name.startswith("camera") else 24), namespace=root.name)
                for name in ("camera0", "camera1", "imu0")]
            registry = root / "capture.json"
            registry.write_text(json.dumps({"streams": {p.spec.stream_id: p.registry_entry() for p in publishers}}))
            spec = ServiceSpec("capture_service", [], stream_activity_timeout_s=0.8,
                               stream_activity_stable_s=0.2)
            launcher = Launcher(SimpleNamespace(log_dir=folder,
                                               gate=SimpleNamespace(command=["gate", "--stereo-max-delta-ms", str(max_delta_ms)]),
                                               root_project=str(Path(__file__).resolve().parents[2])))
            launcher.running = [SimpleNamespace(spec=spec, process=SimpleNamespace(poll=lambda: None))]
            stop = threading.Event()

            def produce():
                count = 0
                while not stop.is_set():
                    stamp = time.monotonic_ns()
                    publishers[0].publish(b"1234", timestamp_ns=stamp)
                    publishers[1].publish(b"1234", timestamp_ns=stamp + skew_ns)
                    publishers[2].publish(struct.pack("<6f", 0, 0, 0, 0, 9.81, 0), timestamp_ns=stamp)
                    count += 1
                    if burst and count == 4:
                        break
                    stop.wait(0.01)

            thread = threading.Thread(target=produce)
            thread.start()
            try:
                waits = [WaitStream(str(registry), p.spec.stream_id) for p in publishers]
                began = time.monotonic()
                if skew_ns > max_delta_ms * 1_000_000 or burst:
                    with self.assertRaisesRegex(RuntimeError, "no fresh data"):
                        launcher.wait_for_stream_activity(spec, waits)
                    self.assertGreater(publishers[0].seq, 0)
                    self.assertGreater(publishers[1].seq, 0)
                else:
                    launcher.wait_for_stream_activity(spec, waits)
                    self.assertGreaterEqual(time.monotonic() - began, 0.2)
            finally:
                stop.set()
                thread.join(1)
                for publisher in publishers:
                    publisher.close()
                    (Path("/dev/shm") / publisher.shm_name).unlink(missing_ok=True)

    def test_continuous_synchronized_capture_is_ready_after_stabilization(self):
        self.check_capture()

    def test_separate_fresh_cameras_without_stereo_sync_are_not_ready(self):
        self.check_capture(skew_ns=5_000_000)

    def test_startup_burst_followed_by_stall_is_not_ready(self):
        self.check_capture(burst=True)

    def test_gate_stereo_tolerance_is_used_during_preparation(self):
        self.check_capture(skew_ns=5_000_000, max_delta_ms=10)


if __name__ == "__main__":
    unittest.main()
