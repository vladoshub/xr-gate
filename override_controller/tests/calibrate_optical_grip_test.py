#!/usr/bin/env python3
"""Synthetic end-to-end checks; these do not replace a real camera test."""
import contextlib
from copy import deepcopy
import hashlib
import io
import json
from pathlib import Path
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock
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


def records(side, number, device, mount=None, pure_yaw=False, world_tilt=None, imu_period_ms=40, optical_period_ms=30):
    rng = np.random.default_rng(1067 + number)
    offset = device.get("orientation_offset", {})
    old = R.from_quat(offset["quaternion_xyzw"]) if offset.get("enabled") else R.identity()
    world_tilt = R.identity() if world_tilt is None else world_tilt
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
        imu_ms = ((tick_ms - 40) // imu_period_ms) * imu_period_ms
        opt_ms = ((tick_ms - 60) // optical_period_ms) * optical_period_ms
        # Diagnostics already contain the configured provider basis. Their
        # canonical world is +Y up, irrespective of the native sensor axes.
        base_imu = attitude(imu_ms)
        force = base_imu.inv().apply([0, 9.80665, 0])
        gyro = (base_imu.inv() * attitude(imu_ms + 1)).as_rotvec() / .001
        imu = world_tilt * base_imu
        if offset.get("enabled"):
            if offset.get("multiply_order", "post") in ("pre", "world"):
                imu = old * imu
            else:
                imu = imu * old
                force = old.inv().apply(force)
                gyro = old.inv().apply(gyro)
        if opt_ms not in optical_cache:
            noise = R.from_rotvec(rng.normal(0, np.radians(.15), 3))
            optical_cache[opt_ms] = noise * world * attitude(opt_ms) * mount
        origin = 10_000_000_000 + number * 10_000_000_000
        text = lambda r: "(" + ",".join(str(x) for x in r.as_quat()) + ")"
        fields = {"controller_tick_ns": str(origin + tick_ms * 1_000_000),
            "controller_optical_source_ns": str(origin + opt_ms * 1_000_000),
            prefix + "imu_orientation_ns": str(origin + imu_ms * 1_000_000),
            prefix + "imu_active": "yes", prefix + "imu_data_flags": "8", prefix + "imu_q": text(imu),
            prefix + "imu_sample_ns": str(origin + imu_ms * 1_000_000),
            prefix + "imu_sample_flags": "3",
            prefix + "imu_force": "(" + ",".join(str(x) for x in force) + ")",
            prefix + "imu_gyro": "(" + ",".join(str(x) for x in gyro) + ")",
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

    def test_generic_bases_and_inversions_are_not_applied_twice(self):
        for transform in [
            {}, {"enabled": False}, {"enabled": True},
            {"enabled": True, "invert_x": True, "basis_rotation": {"rx_deg": -90, "ry_deg": 37}},
            {"enabled": True, "invert_z": True, "basis_rotation": {"rx_deg": 90, "ry_deg": -21}},
            {"enabled": True, "invert_x": True, "invert_y": True,
             "basis_rotation": {"rx_deg": -90, "rz_deg": 180}},
            {"enabled": True, "basis_rotation": {"rx_deg": 23, "ry_deg": -42, "rz_deg": 17}},
        ]:
            with self.subTest(transform=transform):
                cfg = config_fixture()
                cfg["devices"] = [cfg["devices"][1]]
                cfg["devices"][0]["orientation_transform"] = transform
                device = wizard.load_devices(cfg, ["right"])["right"]
                frame = records("right", 1, device)
                data = wizard.make_pairs(frame, "right", device["orientation_offset"])
                mount, _, _ = wizard.fit_mount([data])
                error = np.degrees((mount.inv() * MOUNTS["right"]).magnitude())
                self.assertLess(error, .5)
                self.assertLess(data.gravity["up_error_deg"], .01)

    def test_gravity_rejects_bad_world_basis_and_stale_or_missing_force(self):
        device = self.devices["right"]
        frames = records("right", 1, device, world_tilt=R.from_euler("x", 35, degrees=True))
        with self.assertRaisesRegex(ValueError, "отклонена от \\+Y"):
            wizard.make_pairs(frames, "right", device["orientation_offset"])
        for mode in ("missing", "stale", "accelerating", "turning", "no_accel_flag", "no_gyro_flag"):
            with self.subTest(mode=mode):
                frames = records("right", 1, device)
                for frame in frames:
                    f = frame["fields"]
                    if mode == "missing":
                        f.pop("controller_right_imu_force")
                    elif mode == "stale":
                        f["controller_right_imu_sample_ns"] = str(int(f["controller_right_imu_sample_ns"]) - 100_000_000)
                    elif mode == "accelerating":
                        f["controller_right_imu_force"] = "(0,25,0)"
                    elif mode == "turning":
                        f["controller_right_imu_gyro"] = "(0,2,0)"
                    elif mode == "no_accel_flag":
                        f["controller_right_imu_sample_flags"] = "1"
                    else:
                        f["controller_right_imu_sample_flags"] = "2"
                with self.assertRaisesRegex(ValueError, "проверки вертикали"):
                    wizard.make_pairs(frames, "right", device["orientation_offset"])

    def test_gravity_with_pre_post_and_disabled_offsets(self):
        for side, order in (("left", "pre"), ("right", "post"), ("right", "disabled")):
            with self.subTest(side=side, order=order):
                device = deepcopy(self.devices[side])
                if order == "disabled":
                    device["orientation_offset"] = {}
                frames = records(side, 1, device)
                gravity = wizard.gravity_reference(frames, side, device["orientation_offset"])
                self.assertLess(gravity["up_error_deg"], .001)
                self.assertLess(gravity["spread_p90_deg"], .001)

    def test_current_basis_validation_and_selected_side(self):
        cfg = config_fixture()
        cfg["devices"] = [cfg["devices"][1]]
        self.assertEqual(set(wizard.load_devices(cfg, ["right"])), {"right"})
        with self.assertRaisesRegex(ValueError, "imu_side=left"):
            wizard.load_devices(cfg)
        for transform in [[], {"enabled": "true"}, {"enabled": True, "invert_x": 1},
                          {"basis_rotation": []}, {"basis_rotation": {"rx_deg": float("nan")}}]:
            cfg["devices"][0]["orientation_transform"] = transform
            with self.subTest(transform=transform), self.assertRaises(ValueError):
                wizard.load_devices(cfg, ["right"])

    def test_source_time_at_different_imu_and_optical_rates(self):
        device = self.devices["right"]
        for imu_ms, optical_ms in ((2, 30), (10, 40), (40, 30)):
            with self.subTest(imu_ms=imu_ms, optical_ms=optical_ms):
                frames = records("right", 1, device, imu_period_ms=imu_ms, optical_period_ms=optical_ms)
                data = wizard.make_pairs(frames, "right", device["orientation_offset"])
                mount, _, _ = wizard.fit_mount([data])
                self.assertLess(np.degrees((mount.inv() * MOUNTS["right"]).magnitude()), .5)

    def test_single_hand_generic_replay_apply_and_changed_basis_guard(self):
        cfg_data = config_fixture()
        cfg_data["devices"][1]["orientation_transform"] = {"enabled": False}
        cfg_data["devices"][1]["fingerprint"] = {"backend": "custom_serial", "uniq": "imu-42"}
        device = wizard.load_devices(cfg_data, ["right"])["right"]
        with tempfile.TemporaryDirectory() as directory:
            folder = Path(directory)
            cfg = folder / "custom-controller.json"
            cfg.write_text(json.dumps(cfg_data))
            original = cfg.read_bytes()
            recording = folder / "recording.jsonl"
            with recording.open("w") as f:
                f.write(json.dumps({"schema": wizard.RECORDING_SCHEMA,
                    "selected_sides": ["right"], "devices": {"right": device},
                    "config_sha256": hashlib.sha256(original).hexdigest()}) + "\n")
                for n in (1, 2, 3):
                    for frame in records("right", n, device):
                        f.write(json.dumps(frame) + "\n")
            out = folder / "result"
            with contextlib.redirect_stdout(io.StringIO()):
                result = wizard.main(["--replay", str(recording), "--config", str(cfg), "--out", str(out)])
            self.assertEqual(result, 0)
            candidate = out / "calibration_candidate.json"
            data = json.loads(candidate.read_text())
            self.assertEqual(data["schema"], wizard.CANDIDATE_SCHEMA)
            self.assertEqual([d["side"] for d in data["devices"]], ["right"])
            self.assertNotIn("session_world_yaw", data["devices"][0])
            runtime = folder / "runtime"
            for relative, marker in [("bin/override_controller/override_controller", "optical_grip_vectors_revision=1"),
                    ("bin/runtime_adapters/xr_runtime_adapter/xr_runtime_adapter", "controller_left_yaw_reference_ns=")]:
                p = runtime / relative
                p.parent.mkdir(parents=True, exist_ok=True)
                p.write_text(marker)
            self.assertEqual(apply(cfg, candidate, runtime, False, True)["status"], "ready")
            for key, value, error in [("orientation_transform", {"enabled": True}, "orientation_transform"),
                                      ("fingerprint", {"backend": "custom_serial", "uniq": "other"}, "fingerprint")]:
                changed = deepcopy(cfg_data)
                changed["devices"][1][key] = value
                cfg.write_text(json.dumps(changed))
                with self.assertRaisesRegex(ValueError, error):
                    apply(cfg, candidate, runtime, False, True)
            cfg.write_bytes(original)
            with mock.patch("apply_optical_grip_calibration.require_stopped", side_effect=ValueError("runtime running")):
                with self.assertRaisesRegex(ValueError, "runtime running"):
                    apply(cfg, candidate, runtime, True, True)
            self.assertEqual(cfg.read_bytes(), original)
            self.assertFalse(list(folder.glob("*.bak")))
            applied = apply(cfg, candidate, runtime, True, True)
            updated = json.loads(cfg.read_text())
            self.assertEqual(updated["devices"][0], cfg_data["devices"][0])
            self.assertEqual(updated["keep_acceleration_calibration"], cfg_data["keep_acceleration_calibration"])
            self.assertEqual(updated["devices"][1]["orientation_transform"], {"enabled": False})
            self.assertEqual(Path(applied["backup"]).read_bytes(), original)
            self.assertEqual(apply(cfg, candidate, runtime, True, True)["status"], "already_applied")
            # Idempotency must not hide a subsequent basis/physical-device change.
            updated["devices"][1]["orientation_transform"] = {"enabled": True}
            cfg.write_text(json.dumps(updated))
            with self.assertRaisesRegex(ValueError, "orientation_transform"):
                apply(cfg, candidate, runtime, True, True)

    def test_live_cli_single_hand_capture_keeps_generic_snapshots(self):
        cfg_data = config_fixture()
        cfg_data["devices"] = [cfg_data["devices"][1]]
        cfg_data["devices"][0]["orientation_transform"] = {"enabled": True, "invert_x": True,
            "basis_rotation": {"rx_deg": -90, "ry_deg": 23}}
        device = wizard.load_devices(cfg_data, ["right"])["right"]
        selected = []
        def collect(reader, side, number, seconds, settle, sink):
            selected.append(side)
            frames = records(side, number, device)
            for frame in frames:
                sink.write(json.dumps(frame) + "\n")
            return frames
        with tempfile.TemporaryDirectory() as directory:
            folder = Path(directory)
            cfg = folder / "controller.json"
            cfg.write_text(json.dumps(cfg_data))
            original = cfg.read_bytes()
            log = folder / "runtime.log"
            log.write_text("runtime_controller_left_imu_orientation_transform: enabled=true\n")
            out = folder / "result"
            with mock.patch("builtins.input", return_value=""), mock.patch.object(wizard, "collect", collect), \
                    contextlib.redirect_stdout(io.StringIO()):
                result = wizard.main(["--config", str(cfg), "--side", "right", "--log", str(log), "--out", str(out)])
            self.assertEqual(result, 0)
            self.assertEqual(selected, ["right"] * 3)
            self.assertEqual(cfg.read_bytes(), original)
            header = json.loads((out / "recording.jsonl").read_text().splitlines()[0])
            self.assertEqual(header["schema"], wizard.RECORDING_SCHEMA)
            self.assertEqual(header["selected_sides"], ["right"])
            self.assertEqual(header["devices"]["right"]["orientation_transform"], device["orientation_transform"])
            candidate = json.loads((out / "calibration_candidate.json").read_text())
            self.assertEqual(candidate["devices"][0]["expected_orientation_transform"], device["orientation_transform"])
            self.assertGreater(candidate["devices"][0]["gravity_validation"]["samples"], 10)

    def test_legacy_replay_without_force_still_uses_original_guard(self):
        device = self.devices["right"]
        frames = deepcopy(self.frames["right"])
        for values in frames.values():
            for frame in values:
                for field in ("imu_force", "imu_gyro", "imu_sample_ns", "imu_sample_flags"):
                    frame["fields"].pop("controller_right_" + field)
        entry, result = wizard.assess(frames, device, self.limits, legacy_recording=True)
        self.assertTrue(result["stable"])
        self.assertIsNone(entry["gravity_validation"])
        entry, result = wizard.assess(frames, device, self.limits)
        self.assertIsNone(entry)
        changed = deepcopy(device)
        changed["orientation_transform"] = {"enabled": False}
        entry, result = wizard.assess(frames, changed, self.limits, legacy_recording=True)
        self.assertIsNone(entry)
        self.assertIn("Старая запись", result["rounds"][0]["rejected"])

    def test_selected_side_does_not_require_other_runtime_transform_disabled(self):
        with tempfile.TemporaryDirectory() as directory:
            log = Path(directory) / "runtime.log"
            log.write_text("runtime_controller_left_imu_orientation_transform: enabled=true\n")
            reader = wizard.LiveLog(log, ["right"])
            reader.close()
            with self.assertRaisesRegex(ValueError, "runtime transform"):
                wizard.LiveLog(log, ["left"])

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
