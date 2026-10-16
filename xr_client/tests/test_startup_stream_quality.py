"""A camera quality gate must reject stopped streams, including after a burst."""
import sys
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from xr_client.tools import xr_startup_gate as gate


class StreamQualityTest(unittest.TestCase):
    def run_quality(self, frames, *, stall_timeout=2.0, defective=False):
        clock = SimpleNamespace(now=0.0)
        frames = iter(frames)

        def read_next(timeout_s):
            step = min(0.05, timeout_s)
            clock.now += step
            if next(frames, False):
                return SimpleNamespace(cam0=object(), cam1=object())
            return None

        cfg = gate.StartupGateConfig(
            quality=gate.StreamQualityGateConfig(enabled=True, no_frame_timeout_s=stall_timeout),
            print_every=0)
        with patch.object(gate.time, "monotonic", side_effect=lambda: clock.now), \
             patch.object(gate, "compute_image_health", return_value=gate.ImageHealth()), \
             patch.object(gate, "image_defective", return_value=defective), \
             patch("builtins.print"):
            return gate.run_stream_quality_gate(SimpleNamespace(read_next=read_next), cfg)

    def test_never_started_cameras_fail_without_waiting_full_window(self):
        result = self.run_quality([])
        self.assertFalse(result.passed)
        self.assertEqual(result.reason, "no-frames")
        self.assertLessEqual(result.quality_window_s, 2.01)

    def test_enough_cached_frames_followed_by_disconnect_cannot_pass(self):
        result = self.run_quality([True] * 25)
        self.assertGreaterEqual(result.frames_seen, 20)
        self.assertFalse(result.passed)
        self.assertEqual(result.reason, "stream-stalled")
        self.assertLess(result.quality_window_s, 5)

    def test_healthy_stream_still_needs_full_window(self):
        result = self.run_quality([True] * 200)
        self.assertTrue(result.passed)
        self.assertGreaterEqual(result.frames_seen, 20)
        self.assertAlmostEqual(result.quality_window_s, 5)

    def test_defective_frame_threshold_is_preserved(self):
        result = self.run_quality([True] * 200, defective=True)
        self.assertFalse(result.passed)
        self.assertEqual(result.reason, "too-many-defective-frames")
        self.assertGreater(result.quality_defective, 5)
        self.assertAlmostEqual(result.quality_window_s, 5)

    def test_stall_timeout_can_be_disabled(self):
        result = self.run_quality([True] * 25, stall_timeout=0)
        self.assertTrue(result.passed)
        self.assertAlmostEqual(result.quality_window_s, 5)


if __name__ == "__main__":
    unittest.main()
