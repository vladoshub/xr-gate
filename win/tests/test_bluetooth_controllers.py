import copy
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("bluetooth_controllers", ROOT / "override_controller/scripts/linux/bluetooth_controllers.py")
bt = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(bt)
TEMPLATE = ROOT / "override_controller/configs/empty_gearvr.json"


class BluetoothControllersTest(unittest.TestCase):
    def setUp(self):
        self.config = json.loads(TEMPLATE.read_text())
        self.left = dict(address="AA:BB:CC:DD:EE:01", name="Same controller name")
        self.right = dict(address="AA:BB:CC:DD:EE:02", name="Same controller name")

    def test_two_identical_names_keep_independent_mac_addresses_and_buttons(self):
        self.config["devices"][0]["orientation_offset"] = {"enabled": True, "custom": "calibration"}
        result = bt.configure(self.config, self.config, dict(left=self.left, right=self.right))
        self.assertEqual(bt.selected_sides(result), {"left": self.left["address"], "right": self.right["address"]})
        self.assertEqual(result["bindings"], self.config["bindings"])
        self.assertEqual(result["devices"][0]["orientation_offset"], self.config["devices"][0]["orientation_offset"])
        self.assertNotEqual(result["devices"][0]["stable_hash"], result["devices"][1]["stable_hash"])
        self.assertEqual(result["devices"][0]["uniq"], "gearvr_ble:" + self.left["address"])
        self.assertEqual(self.config["devices"][0]["phys"], "")

    def test_omitted_side_has_no_bindings_or_imu_and_can_be_added_later(self):
        for selected_side, other_side in (("left", "right"), ("right", "left")):
            selection = {selected_side: self.left, other_side: None}
            single = bt.configure(self.config, self.config, selection)
            self.assertEqual({d["imu_side"] for d in single["devices"]}, {selected_side})
            for group in bt.BINDING_GROUPS:
                self.assertFalse(any(b.get("side") == other_side for b in single.get(group, [])))
            both = bt.configure(single, self.config, dict(left=self.left, right=self.right))
            self.assertEqual({d["imu_side"] for d in both["devices"]}, {"left", "right"})
            ids = {d["id"] for d in both["devices"]}
            self.assertEqual(len(ids), 2)
            self.assertTrue(all(b["device_id"] in ids for b in both["bindings"]))

    def test_rejects_one_physical_controller_for_both_hands(self):
        with self.assertRaisesRegex(ValueError, "different"):
            bt.configure(self.config, self.config, dict(left=self.left, right=self.left))
        with self.assertRaisesRegex(ValueError, "at least one"):
            bt.configure(self.config, self.config, dict(left=None, right=None))

    def test_pairing_failure_never_rewrites_the_learned_config(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "controllers.json"
            original = json.dumps(self.config).encode()
            path.write_bytes(original)
            info = dict(self.left, paired=True, trusted=True, connected=False)
            with patch.object(bt, "device_info", return_value=info), patch.object(bt, "ctl", side_effect=RuntimeError("connect failed")):
                with self.assertRaisesRegex(RuntimeError, "connect failed"):
                    bt.pair_and_save(path, TEMPLATE, self.left["address"], "none")
            self.assertEqual(path.read_bytes(), original)
            self.assertEqual(list(path.parent.glob("*.before-bluetooth-*")), [])

    def test_success_saves_one_side_and_backs_up_previous_config(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "controllers.json"
            original = json.dumps(self.config).encode()
            path.write_bytes(original)
            info = dict(self.left, paired=True, trusted=True, connected=True)
            with patch.object(bt, "device_info", return_value=info), patch.object(bt, "ctl", return_value=""):
                result = bt.pair_and_save(path, TEMPLATE, self.left["address"], "none")
            saved = json.loads(path.read_text())
            self.assertEqual(bt.selected_sides(saved), {"left": self.left["address"], "right": None})
            self.assertEqual(result["selected"]["right"], None)
            self.assertEqual(next(path.parent.glob("*.before-bluetooth-*")).read_bytes(), original)

    def test_missing_adapter_is_an_error_even_when_bluetoothctl_exits_zero(self):
        with patch.object(bt, "ctl", return_value="No default controller available"):
            with self.assertRaisesRegex(RuntimeError, "No Bluetooth controller"):
                bt.discover(self.config)

    def test_saved_identity_uses_the_same_alias_as_the_ble_provider(self):
        with patch.object(bt, "ctl", return_value="Device AA:BB:CC:DD:EE:01\n Name: Original name\n Alias: My left controller\n Paired: yes\n"):
            info = bt.device_info(self.left["address"])
        self.assertEqual(info["name"], "My left controller")
        self.assertTrue(info["paired"])

    def test_name_based_discovery_disambiguates_duplicates_and_keeps_saved_sides(self):
        config = bt.configure(self.config, self.config, dict(left=self.left, right=self.right))
        def ctl(*args, **_):
            if args == ("list",):
                return "Controller AA:AA:AA:AA:AA:AA USB adapter [default]"
            if args == ("devices",):
                return "\n".join("Device " + d["address"] + " " + d["name"] for d in (self.left, self.right))
            return ""
        with patch.object(bt, "ctl", side_effect=ctl), patch.object(bt, "command", return_value=""):
            found = bt.discover(config)
        self.assertEqual(len(found["devices"]), 2)
        self.assertNotEqual(found["devices"][0]["label"], found["devices"][1]["label"])
        self.assertEqual(found["selected"], bt.selected_sides(config))


if __name__ == "__main__":
    unittest.main()
