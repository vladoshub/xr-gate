#!/usr/bin/env python3
import contextlib
import io
import json
import math
from pathlib import Path
import random
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "debug"))
import calibrate_controller_accelerometer as tool


def known_sensor_captures():
    random_source = random.Random(27)
    bias = [-0.107, -0.078, 0.431]
    scale = [0.9902, 0.9945, 0.9861]
    captures = {}
    for index, pose in enumerate(tool.POSES):
        axis = "XYZ".index(pose[1])
        sign = 1 if pose[0] == "+" else -1
        # Slightly tilted poses are allowed: fit uses the gravity norm,
        # rather than assuming each pose is perfectly on one sensor axis.
        force = [0.32 + index * 0.013, -0.23 - index * 0.007, 0.41]
        force[axis] = sign * math.sqrt(9.80665 ** 2 - sum(force[i] ** 2 for i in range(3) if i != axis))
        captures[pose] = [[force[i] / scale[i] + bias[i] + random_source.gauss(0, 0.005)
                          for i in range(3)] for _ in range(600)]
    return captures, bias, scale


def row(ns=1_000_000_000, flags=3, gyro=0, epoch=1):
    return dict(sample_index=0, imu_status=3, capability_flags=3, sample_flags=flags,
                data_flags=3, device_id="0000000000000123", ax=0, ay=9.80665, az=0,
                gx=gyro, gy=0, gz=0, sample_ns=ns, epoch=epoch, side="left")


class CalibrationTest(unittest.TestCase):
    def test_recovers_known_bias_scale_from_tilted_noisy_six_poses(self):
        captures, bias, scale = known_sensor_captures()
        calibration, quality = tool.fit_calibration(captures)
        for actual, expected in zip(calibration["accelerometer_bias_m_s2"], bias):
            self.assertAlmostEqual(actual, expected, delta=0.002)
        for actual, expected in zip(calibration["accelerometer_scale"], scale):
            self.assertAlmostEqual(actual, expected, delta=0.0004)
        self.assertLess(quality["gravity_rms_m_s2"], 0.01)
        self.assertEqual(quality["samples"], 3600)

    def test_rejects_missing_repeated_short_nonfinite_and_moving_poses(self):
        captures, _, _ = known_sensor_captures()
        bad = dict(captures); del bad["-Z"]
        with self.assertRaises(ValueError): tool.fit_calibration(bad)
        bad = dict(captures); bad["-Z"] = captures["+Z"]
        with self.assertRaises(ValueError): tool.fit_calibration(bad)
        bad = dict(captures); bad["+X"] = captures["+X"][:99]
        with self.assertRaises(ValueError): tool.fit_calibration(bad)
        bad = dict(captures); bad["+X"] = [[float("nan"), 0, 0]] * 200
        with self.assertRaises(ValueError): tool.fit_calibration(bad)
        bad = dict(captures); bad["+X"] = [[9.8, i % 2, 0] for i in range(200)]
        with self.assertRaises(ValueError): tool.fit_calibration(bad)

    def test_rejects_unphysical_bias_and_singular_fit(self):
        captures = {pose: [[(9.80665 if pose[0] == "+" else -9.80665)
                            * (axis == "XYZ".index(pose[1])) + (2.1 if axis == 0 else 0)
                            for axis in range(3)]] * 200 for pose in tool.POSES}
        with self.assertRaises(ValueError): tool.fit_calibration(captures)
        with self.assertRaises(ValueError): tool.solve_linear([[1, 1], [2, 2]], [1, 2])

    def test_basis_rotation_and_reflection_are_undone_before_fit(self):
        device = {"orientation_transform": {"enabled": True, "invert_y": True,
                                             "basis_rotation": {"rx_deg": -90}}}
        # Raw native (1,2,3) -> invert Y (1,-2,3) -> Rx(-90) (1,3,2).
        actual = tool.inverse_basis((1, 3, 2), device)
        for value, expected in zip(actual, (1, 2, 3)):
            self.assertAlmostEqual(value, expected)
        device["orientation_transform"]["invert_y"] = False
        actual = tool.inverse_basis((1, 3, -2), device)
        for value, expected in zip(actual, (1, 2, 3)):
            self.assertAlmostEqual(value, expected)

    def test_calibrated_or_wrong_device_data_cannot_be_fitted_as_raw(self):
        device = {"stable_hash": "0000000000000123"}
        self.assertEqual(tool.native_sample(row(), device)[0], 1_000_000_000)
        with self.assertRaisesRegex(ValueError, "correction is active"):
            tool.native_sample(row(flags=3 | 64), device)
        bad = row(); bad["capability_flags"] = 1
        with self.assertRaisesRegex(ValueError, "accelerometer"):
            tool.native_sample(bad, device)
        bad = row(); bad["device_id"] = "0000000000000456"
        with self.assertRaisesRegex(ValueError, "another device"):
            tool.native_sample(bad, device)

    def test_capture_does_not_count_duplicate_samples_and_restarts_on_epoch_change(self):
        data = [row(ns=1_000_000_000 + i * 10_000_000, gyro=0.5) for i in range(50)]
        data += [row(ns=1_500_000_000 + i * 10_000_000) for i in range(60)]
        for i in range(220):
            value = row(ns=3_000_000_000 + i * 10_000_000, epoch=2)
            data += [value, value.copy()]
        class Reader:
            def rows(self): return iter(data)
        with contextlib.redirect_stdout(io.StringIO()):
            samples = tool.capture_pose(Reader(), {"imu_side": "left", "stable_hash": "123"},
                                        "+Y", 9.80665, 2, 10)
        self.assertEqual(len(samples), 201)

    def test_config_update_uses_device_id_and_preserves_other_devices_bindings_and_fields(self):
        config = {"devices": [{"id": 42}, {"id": 7}], "bindings": [{"device_id": 7, "action": "grip"}],
                  "custom": {"preserve": True}, "imu_calibrations": [
                      {"device_id": 7, "enabled": True, "accelerometer_bias_m_s2": [.1, .2, .3],
                       "accelerometer_scale": [1, 1, 1]}]}
        original = json.dumps(config)
        calibration, _ = tool.fit_calibration(known_sensor_captures()[0])
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "profile.json"
            path.write_text(original)
            backup = tool.write_config(path, original, 42, calibration)
            updated = json.loads(path.read_text())
            self.assertEqual(backup.read_text(), original)
            for key in ("devices", "bindings", "custom"):
                self.assertEqual(updated[key], config[key])
            self.assertEqual(updated["imu_calibrations"][0], config["imu_calibrations"][0])
            self.assertEqual(updated["imu_calibrations"][1]["device_id"], 42)
            self.assertNotIn("uuid", updated["imu_calibrations"][1])
            with self.assertRaisesRegex(ValueError, "changed during capture"):
                tool.write_config(path, original, 42, calibration)

    def test_duplicate_or_unknown_ids_are_rejected_before_capture(self):
        with self.assertRaises(ValueError): tool.config_device({"devices": [{"id": 1}]}, 2)
        with self.assertRaises(ValueError):
            tool.config_device({"devices": [{"id": 1}], "imu_calibrations":
                                [{"device_id": 1}, {"device_id": 1}]}, 1)


if __name__ == "__main__":
    unittest.main()
