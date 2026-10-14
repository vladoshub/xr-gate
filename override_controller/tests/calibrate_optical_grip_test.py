#!/usr/bin/env python3
"""Synthetic end-to-end checks; these do not replace a real camera test."""
import contextlib
import hashlib
import io
import json
from pathlib import Path
import tempfile
from types import SimpleNamespace
import unittest
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "debug"))

import numpy as np
from scipy.spatial.transform import Rotation as R
import calibrate_optical_grip as wizard
from apply_optical_grip_calibration import apply


def config_fixture():
    devices = []
    for side, device_id, order in (("left", 11, "pre"), ("right", 22, "post")):
        devices.append({"id": device_id, "imu_side": side, "keep_binding": "unchanged",
            "orientation_transform": {"enabled": True, "invert_x": False,
                "invert_y": False, "invert_z": False,
                "basis_rotation": {"rx_deg": -90, "ry_deg": 0, "rz_deg": 0}},
            "orientation_offset": {"enabled": True, "multiply_order": order,
                "quaternion_xyzw": R.from_rotvec([.08, -.13, .12]).as_quat().tolist()}})
    return {"devices": devices, "keep_acceleration_calibration": [1, 2, 3]}


MOUNTS = {"left": R.from_rotvec([.35, -.2, .4]), "right": R.from_rotvec([-.2, .3, .5])}


def records(side, number, device, mount=None, pure_yaw=False):
    rng = np.random.default_rng(1067 + number)
    old = R.from_quat(device["orientation_offset"]["quaternion_xyzw"])
    mount = MOUNTS[side] if mount is None else mount
    world = R.from_rotvec([0, .3 + number * .005, 0])
    prefix = "controller_" + side + "_"
    def attitude(ms):
        t = max(0, ms) / 1000
        if pure_yaw:
            return R.from_rotvec([0, .7 * np.sin(t * 2), 0])
        phase = min(3, int(t / 2))
        angles = [10., 10., 0.]
        if phase > 0:
            axis = {1: 0, 2: 2, 3: 1}[phase]
            angles[axis] += 40 * np.sin((t % 2) * np.pi * 2)
        return R.from_euler("xyz", angles, degrees=True)
    frames = []
    optical_cache = {}
    for tick_ms in range(0, 8001, 10):
        imu_ms = ((tick_ms - 40) // 40) * 40
        opt_ms = ((tick_ms - 60) // 30) * 30
        imu = attitude(imu_ms)
        imu = old * imu if device["orientation_offset"]["multiply_order"] == "pre" else imu * old
        if opt_ms not in optical_cache:
            noise = R.from_rotvec(rng.normal(0, np.radians(.15), 3))
            optical_cache[opt_ms] = noise * world * attitude(opt_ms) * mount
        origin = 10_000_000_000 + number * 10_000_000_000
        text = lambda r: "(" + ",".join(str(x) for x in r.as_quat()) + ")"
        fields = {"controller_tick_ns": str(origin + tick_ms * 1_000_000),
            "controller_optical_source_ns": str(origin + opt_ms * 1_000_000),
            prefix + "imu_orientation_ns": str(origin + imu_ms * 1_000_000),
            prefix + "imu_active": "yes", prefix + "imu_data_flags": "8", prefix + "imu_q": text(imu),
            prefix + "raw_optical_q": text(optical_cache[opt_ms]),
            prefix + "raw_optical_status": "1", prefix + "accepted_optical_status": "1",
            prefix + "imu_epoch": "2", "hmd_reset_counter": "1", "hmd_q": "(0,0,0,1)"}
        phase = wizard.PHASES[min(3, tick_ms // 2000)][0]
        frames.append({"kind": "frame", "side": side, "round": number, "phase": phase, "fields": fields})
    return frames


class CalibrationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.config = config_fixture()
        cls.devices = wizard.load_devices(cls.config)
        cls.frames = {side: {n: records(side, n, cls.devices[side]) for n in (1, 2, 3)}
                      for side in ("left", "right")}
        cls.limits = SimpleNamespace(min_rounds=3, stable_deg=3,
                                    max_yaw_p90_deg=12, max_orientation_median_deg=25)

    def test_independent_mounts_remove_old_offsets_and_use_source_time(self):
        for side in ("left", "right"):
            entry, report = wizard.assess(self.frames[side], self.devices[side], self.limits)
            self.assertTrue(report["stable"], report)
            error = np.degrees((R.from_quat(entry["quaternion_xyzw"]).inv() * MOUNTS[side]).magnitude())
            self.assertLess(error, .5)
            self.assertLess(report["verification_metrics"]["yaw_p90_deg"], 1)
            self.assertEqual(entry["device_id"], self.devices[side]["id"])
            self.assertNotIn("session_world_yaw", entry)

    def test_unobservable_yaw_only_motion_is_rejected(self):
        frames = records("right", 1, self.devices["right"], pure_yaw=True)
        with self.assertRaisesRegex(ValueError, "разных осей"):
            wizard.make_pairs(frames, "right", self.devices["right"]["orientation_offset"])

    def test_changing_grip_is_not_reported_as_stable(self):
        frames = {n: records("right", n, self.devices["right"],
            R.from_rotvec([-.2 + n * .15, .3, .5])) for n in (1, 2, 3)}
        entry, report = wizard.assess(frames, self.devices["right"], self.limits)
        self.assertIsNone(entry)
        self.assertFalse(report["stable"])
        self.assertGreater(report["mount_spread_deg"], 3)

    def test_held_optics_and_epoch_change_rejected(self):
        frames = records("right", 1, self.devices["right"])
        for f in frames:
            f["fields"]["controller_right_accepted_optical_status"] = "2"
        with self.assertRaises(ValueError):
            wizard.make_pairs(frames, "right", self.devices["right"]["orientation_offset"])
        frames = records("right", 1, self.devices["right"])
        frames[-1]["fields"]["controller_right_imu_epoch"] = "3"
        with self.assertRaisesRegex(ValueError, "перезапустился"):
            wizard.make_pairs(frames, "right", self.devices["right"]["orientation_offset"])

    def test_partial_lines_rotation_and_replay_apply(self):
        with tempfile.TemporaryDirectory() as directory:
            folder = Path(directory)
            log = folder / "runtime.log"
            log.write_text("old data\n")
            reader = wizard.LiveLog(log)
            with log.open("ab") as f:
                f.write(b"[xr_runtime_adapter] frame=1 controller_tick_ns=12")
            self.assertEqual(reader.frames(), [])
            with log.open("ab") as f:
                f.write(b"3\n")
            self.assertEqual(reader.frames()[0]["controller_tick_ns"], "123")
            log.write_text("")
            with self.assertRaisesRegex(ValueError, "перезапущен"):
                reader.frames()
            reader.close()
            log.write_text("runtime_controller_left_imu_orientation_transform: enabled=true\n")
            with self.assertRaisesRegex(ValueError, "runtime transform"):
                wizard.LiveLog(log)

            cfg = folder / "gearvr.json"
            cfg.write_text(json.dumps(self.config))
            original = cfg.read_bytes()
            recording = folder / "recording.jsonl"
            with recording.open("w") as f:
                f.write(json.dumps({"schema": "gearvr_guided_grip_recording_v1",
                    "config_sha256": hashlib.sha256(original).hexdigest(), "devices": self.devices}) + "\n")
                for side in ("right", "left"):
                    for frames in self.frames[side].values():
                        for record in frames:
                            f.write(json.dumps(record) + "\n")
            out = folder / "result"
            with contextlib.redirect_stdout(io.StringIO()):
                result = wizard.main(["--replay", str(recording), "--config", str(cfg), "--out", str(out)])
            self.assertEqual(result, 0)
            candidate = out / "calibration_candidate.json"
            runtime = folder / "runtime"
            for relative, marker in [("bin/override_controller/override_controller", "optical_grip_vectors_revision=1"),
                    ("bin/runtime_adapters/xr_runtime_adapter/xr_runtime_adapter", "controller_left_yaw_reference_ns=")]:
                p = runtime / relative
                p.parent.mkdir(parents=True, exist_ok=True)
                p.write_text(marker)
            self.assertEqual(apply(cfg, candidate, runtime, False, True)["status"], "ready")
            self.assertEqual(cfg.read_bytes(), original)
            with self.assertRaises(ValueError):
                apply(cfg, candidate, runtime, True, False)
            applied = apply(cfg, candidate, runtime, True, True)
            self.assertEqual(Path(applied["backup"]).read_bytes(), original)
            updated = json.loads(cfg.read_text())
            for d in updated["devices"]:
                d["orientation_offset"] = next(x["orientation_offset"] for x in self.config["devices"] if x["id"] == d["id"])
            self.assertEqual(updated, self.config)
            self.assertEqual(apply(cfg, candidate, runtime, True, True)["status"], "already_applied")
            cfg.write_bytes(original + b"\n")
            with self.assertRaisesRegex(ValueError, "Config changed"):
                apply(cfg, candidate, runtime, True, True)
            self.assertEqual(cfg.read_bytes(), original + b"\n")


if __name__ == "__main__":
    unittest.main()
