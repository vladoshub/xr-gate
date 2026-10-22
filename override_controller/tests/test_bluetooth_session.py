from contextlib import redirect_stdout
import importlib.util
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import Mock, patch


ROOT = Path(__file__).resolve().parents[2]
HELPERS = ROOT / "override_controller/scripts/linux"
SPEC = importlib.util.spec_from_file_location("bluetooth_session_helper", HELPERS / "bluetooth_controllers.py")
bt = importlib.util.module_from_spec(SPEC)
with patch.object(sys, "path", [str(HELPERS), *sys.path]):
    SPEC.loader.exec_module(bt)
adapters = sys.modules["select_bluetooth_adapter"]
TEMPLATE = ROOT / "override_controller/configs/empty_gearvr.json"
REAL_POPEN = subprocess.Popen
BUILTIN = "AA:AA:AA:AA:AA:01"
USB = "AA:AA:AA:AA:AA:02"
LEFT = "BB:BB:BB:BB:BB:01"
RIGHT = "BB:BB:BB:BB:BB:02"


def adapter(mac, powered=True):
    return adapters.Adapter("/org/bluez/hci0" if mac == BUILTIN else "/org/bluez/hci1", mac,
                            powered, "Built-in" if mac == BUILTIN else "USB adapter")


class AdapterChoiceTest(unittest.TestCase):
    def test_only_powered_adapter_wins_even_when_builtin_is_first(self):
        with patch.object(bt.BlueZ, "adapters", return_value=[adapter(BUILTIN, False), adapter(USB)]):
            self.assertEqual(bt.choose_adapter().address, USB)

    def test_multiple_powered_adapters_prompt_or_require_explicit_selection(self):
        with patch.object(bt.BlueZ, "adapters", return_value=[adapter(BUILTIN), adapter(USB)]):
            with self.assertRaisesRegex(RuntimeError, "Multiple powered"):
                bt.choose_adapter(interactive=False)
            with patch("builtins.input", return_value="2"), redirect_stdout(io.StringIO()):
                self.assertEqual(bt.choose_adapter(interactive=True).address, USB)
            self.assertEqual(bt.choose_adapter(USB.lower()).address, USB)
            self.assertEqual(bt.choose_adapter("hci1").address, USB)

    def test_zero_powered_or_explicitly_disabled_adapter_does_not_enable_anything(self):
        with patch.object(bt.BlueZ, "adapters", return_value=[adapter(BUILTIN, False), adapter(USB, False)]), \
                patch.object(bt.BlueZ, "set_powered") as set_power:
            for selector in (None, USB):
                with self.subTest(selector=selector), self.assertRaisesRegex(RuntimeError, "powered"):
                    bt.choose_adapter(selector)
            set_power.assert_not_called()

    def test_env_selector_can_be_overridden_and_json_keeps_legacy_fields(self):
        session = Mock()
        session.__enter__ = Mock(return_value=session)
        session.__exit__ = Mock(return_value=False)
        found = {"devices": [], "selected": {"left": None, "right": None}}
        output = io.StringIO()
        with patch.dict(os.environ, {"XR_BLUETOOTH_ADAPTER": BUILTIN}), \
                patch.object(bt, "choose_adapter", return_value=adapter(USB)) as choose, \
                patch.object(bt, "BluetoothSession", return_value=session), \
                patch.object(bt, "discover", return_value=found), redirect_stdout(output):
            self.assertEqual(bt.main(["--config", str(TEMPLATE), "--scan", "--adapter", USB]), 0)
        choose.assert_called_once_with(USB, interactive=False)
        value = json.loads(output.getvalue())
        self.assertEqual(value["devices"], [])
        self.assertEqual(value["selected"], {"left": None, "right": None})
        self.assertEqual(value["adapter"]["address"], USB)


class BluetoothSessionTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.log = self.root / "commands.jsonl"
        self.config = {
            "adapters": {BUILTIN: {"powered": False}, USB: {"powered": True}},
            "devices": {
                BUILTIN: {LEFT: {"name": "Wrong built-in device"}},
                USB: {LEFT: {"name": "Same Gear controller"}, RIGHT: {"name": "Same Gear controller"}},
            },
            "log": str(self.log), "fragmented": True,
        }

        def popen(args, **kwargs):
            self.assertEqual(args, ["bluetoothctl", "--agent", "NoInputNoOutput"])
            env = dict(kwargs.pop("env"), XR_TEST_BLUETOOTH_SESSION=json.dumps(self.config))
            process = REAL_POPEN([sys.executable, str(Path(__file__).parent / "fixtures/fake_bluetoothctl.py")], env=env, **kwargs)
            self.process = process
            return process

        self.popen_patch = patch.object(bt.subprocess, "Popen", side_effect=popen)
        self.popen_patch.start()
        self.addCleanup(self.popen_patch.stop)

    def commands(self):
        return [json.loads(line) for line in self.log.read_text().splitlines()]

    def test_scan_and_pair_both_sides_stay_on_one_selected_adapter(self):
        config_path = self.root / "gearvr.json"
        with bt.BluetoothSession(adapter(USB)) as session:
            process = session.process
            config = json.loads(TEMPLATE.read_text())
            found = bt.discover(config, session, seconds=0.01)
            self.assertEqual({d["address"] for d in found["devices"]}, {LEFT, RIGHT})
            self.assertEqual({d["name"] for d in found["devices"]}, {"Same Gear controller"})
            bt.pair_and_save(config_path, TEMPLATE, LEFT, RIGHT, session)
            self.assertIs(session.process, process)
        self.assertIsNotNone(process.poll())
        saved = json.loads(config_path.read_text())
        self.assertEqual(bt.selected_sides(saved), {"left": LEFT, "right": RIGHT})
        self.assertEqual(len({d["stable_hash"] for d in saved["devices"]}), 2)
        for command in self.commands():
            if command["command"][0] not in ("select", "quit"):
                self.assertEqual(command["adapter"], USB)
            self.assertNotEqual(command["command"][0], "power")

    def test_same_mac_cached_on_both_adapters_uses_selected_adapters_name(self):
        with bt.BluetoothSession(adapter(USB)) as session:
            info = bt.device_info(LEFT, session)
        self.assertEqual(info["name"], "Same Gear controller")
        self.assertNotEqual(info["name"], "Wrong built-in device")

    def test_failed_selection_sends_no_device_commands_and_reaps_process(self):
        for mode in ("reject-select", "select-eof"):
            with self.subTest(mode=mode):
                self.log.unlink(missing_ok=True)
                self.config["mode"] = mode
                with self.assertRaisesRegex(RuntimeError, "not available|closed unexpectedly"):
                    with bt.BluetoothSession(adapter(USB)):
                        self.fail("Selection failure must stop before entering the session")
                self.assertIsNotNone(self.process.poll())
                self.assertTrue(all(c["command"][0] in ("select", "show", "quit") for c in self.commands()))

    def test_adapter_disappearing_after_info_cannot_fall_back_to_builtin_or_rewrite_config(self):
        self.config["mode"] = "drop-after-info"
        path = self.root / "gearvr.json"
        original = TEMPLATE.read_bytes()
        path.write_bytes(original)
        with self.assertRaisesRegex(RuntimeError, "not available"):
            with bt.BluetoothSession(adapter(USB)) as session:
                bt.pair_and_save(path, TEMPLATE, LEFT, "none", session)
        self.assertEqual(path.read_bytes(), original)
        self.assertEqual(list(self.root.glob("*.before-bluetooth-*")), [])
        self.assertFalse(any(c["command"][0] in ("pair", "trust", "connect") for c in self.commands()))
        self.assertIsNotNone(self.process.poll())

    def test_pair_failure_or_false_success_preserves_existing_config(self):
        for mode in ("pair-error", "pair-lies"):
            with self.subTest(mode=mode):
                self.config["mode"] = mode
                path = self.root / "gearvr.json"
                original = TEMPLATE.read_bytes()
                path.write_bytes(original)
                with self.assertRaisesRegex(RuntimeError, "Failed to pair|Pairing did not complete"):
                    with bt.BluetoothSession(adapter(USB)) as session:
                        bt.pair_and_save(path, TEMPLATE, LEFT, "none", session)
                self.assertEqual(path.read_bytes(), original)
                self.assertEqual(list(self.root.glob("*.before-bluetooth-*")), [])
                self.assertIsNotNone(self.process.poll())

    def test_stuck_pair_command_has_a_deadline_and_client_is_closed(self):
        self.config["mode"] = "pair-timeout"
        with self.assertRaisesRegex(RuntimeError, "timed out"):
            with bt.BluetoothSession(adapter(USB)) as session:
                session.ctl("pair", LEFT, seconds=0.05)
        self.assertIsNotNone(self.process.poll())

    def test_scan_interruption_stops_only_selected_clients_scan_and_closes(self):
        with self.assertRaises(KeyboardInterrupt):
            with bt.BluetoothSession(adapter(USB)) as session:
                exchange = session.exchange

                def interrupt(lines, finish, seconds=8, collect=False):
                    if collect:
                        raise KeyboardInterrupt
                    return exchange(lines, finish, seconds, collect)

                with patch.object(session, "exchange", side_effect=interrupt):
                    session.scan(1)
        self.assertIsNotNone(self.process.poll())
        scans = [c for c in self.commands() if c["command"][0] == "scan"]
        self.assertEqual([c["command"] for c in scans], [["scan", "on"], ["scan", "off"]])
        self.assertTrue(all(c["adapter"] == USB for c in scans))


if __name__ == "__main__":
    unittest.main()
