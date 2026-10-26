import argparse
from contextlib import redirect_stderr, redirect_stdout
import importlib.util
import io
import json
import os
from pathlib import Path
import sys
import subprocess
import tempfile
import unittest
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location(
    "select_bluetooth_adapter", ROOT / "override_controller/scripts/linux/select_bluetooth_adapter.py")
bt = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = bt
SPEC.loader.exec_module(bt)


class FakeBus:
    """Exercise the actual busctl protocol and CLI without touching host devices."""
    def __init__(self):
        self.addresses = {"/org/bluez/hci0": "AA:BB:CC:DD:EE:01", "/org/bluez/hci2": "AA:BB:CC:DD:EE:02"}
        self.power = {"/org/bluez/hci0": True, "/org/bluez/hci2": False}
        self.writes = []
        self.commands = []
        self.fail_once = None
        self.ignore_once = None
        self.break_identity = None

    def command(self, args, timeout):
        self.commands.append(args)
        if "GetManagedObjects" in args:
            objects = {path: {bt.INTERFACE: {
                "Address": {"type": "s", "data": address},
                "Powered": {"type": "b", "data": self.power[path]},
                "Name": {"type": "s", "data": "Same PC hostname"},
            }} for path, address in self.addresses.items()}
            objects["/org/bluez/hci0/dev_11_22_33_44_55_66"] = {
                "org.bluez.Device1": {"Name": {"type": "s", "data": "A controller"}}}
            return json.dumps({"type": "a{oa{sa{sv}}}", "data": [objects]})
        if "get-property" in args:
            _, path, interface, prop = args[args.index("get-property") + 1:]
            assert interface == bt.INTERFACE
            if prop == "Address":
                value = self.addresses[path]
                if self.break_identity == path:
                    value = "AA:BB:CC:DD:EE:99"
                return json.dumps({"type": "s", "data": value})
            assert prop == "Powered"
            return json.dumps({"type": "b", "data": self.power[path]})
        if "set-property" in args:
            _, path, interface, prop, signature, value = args[args.index("set-property") + 1:]
            assert (interface, prop, signature) == (bt.INTERFACE, "Powered", "b")
            desired = value == "true"
            self.writes.append((path, desired))
            if self.ignore_once == (path, desired):
                self.ignore_once = None
            else:
                self.power[path] = desired
            if self.fail_once and self.fail_once[:2] == (path, desired):
                error = self.fail_once[2]
                self.fail_once = None
                raise error
            return ""
        raise AssertionError(f"Unexpected command: {args}")


class BluetoothAdaptersTest(unittest.TestCase):
    def setUp(self):
        self.bus = FakeBus()
        self.bluez = bt.BlueZ(2)
        self.command_patch = patch.object(bt, "run_command", self.bus.command)
        self.command_patch.start()
        self.names_patch = patch.object(bt, "hardware_name", side_effect=lambda hci, name, timeout: (
            "Built-in adapter" if hci == "hci0" else "USB adapter", "1234:5678" if hci == "hci2" else ""))
        self.names_patch.start()
        self.addCleanup(self.command_patch.stop)
        self.addCleanup(self.names_patch.stop)

    def cli(self, args, answer=None, env=None, tty=True):
        stdout, stderr = io.StringIO(), io.StringIO()
        with patch.dict(os.environ, env or {}, clear=True), patch.object(sys.stdin, "isatty", return_value=tty), \
                patch("builtins.input", return_value=answer), redirect_stdout(stdout), redirect_stderr(stderr):
            code = bt.main(args)
        return code, stdout.getvalue(), stderr.getvalue()

    def test_menu_uses_hardware_names_and_disambiguates_identical_bluez_names(self):
        code, output, error = self.cli([], answer="2")
        self.assertEqual((code, error), (0, ""))
        self.assertIn("Built-in adapter", output)
        self.assertIn("USB adapter", output)
        self.assertIn("USB 1234:5678", output)
        self.assertNotIn("Same PC hostname", output)
        self.assertEqual(self.bus.power, {"/org/bluez/hci0": False, "/org/bluez/hci2": True})
        self.assertEqual(self.bus.writes, [("/org/bluez/hci2", True), ("/org/bluez/hci0", False)])

    def test_list_is_read_only_even_with_a_saved_selector(self):
        code, output, error = self.cli(["--list"], env={"XR_BLUETOOTH_ADAPTER": "hci2"}, tty=False)
        self.assertEqual((code, error), (0, ""))
        self.assertIn("AA:BB:CC:DD:EE:02", output)
        self.assertEqual(self.bus.writes, [])
        self.assertEqual(len(self.bus.commands), 1)  # A single ObjectManager snapshot.

    def test_direct_mac_selection_works_without_a_terminal(self):
        code, _, error = self.cli(["--adapter", "aa:bb:cc:dd:ee:02"], tty=False)
        self.assertEqual((code, error), (0, ""))
        self.assertTrue(self.bus.power["/org/bluez/hci2"])

    def test_env_selector_and_explicit_override(self):
        code, _, _ = self.cli(["--adapter", "hci0"], env={"XR_BLUETOOTH_ADAPTER": "hci2"}, tty=False)
        self.assertEqual(code, 0)
        self.assertEqual(self.bus.writes, [])
        code, _, _ = self.cli([], env={"XR_BLUETOOTH_ADAPTER": "hci2"}, tty=False)
        self.assertEqual(code, 0)
        self.assertTrue(self.bus.power["/org/bluez/hci2"])

    def test_empty_input_cancels_without_writes(self):
        code, output, _ = self.cli([], answer="")
        self.assertEqual(code, 0)
        self.assertIn("Cancelled", output)
        self.assertEqual(self.bus.writes, [])

    def test_invalid_selection_or_missing_terminal_never_changes_power(self):
        for args, tty in ((["--adapter", "unknown model"], False), ([], False)):
            with self.subTest(args=args):
                code, _, error = self.cli(args, tty=tty)
                self.assertEqual(code, 1)
                self.assertTrue(error)
                self.assertEqual(self.bus.writes, [])

    def test_failure_powering_usb_does_not_disable_builtin(self):
        self.bus.fail_once = ("/org/bluez/hci2", True, RuntimeError("Operation failed: blocked"))
        code, _, error = self.cli(["--adapter", "hci2"])
        self.assertEqual(code, 1)
        self.assertIn("restored", error)
        self.assertTrue(self.bus.power["/org/bluez/hci0"])
        self.assertFalse(self.bus.power["/org/bluez/hci2"])
        self.assertNotIn(("/org/bluez/hci0", False), self.bus.writes)

    def test_partial_failure_restores_both_original_states(self):
        self.bus.fail_once = ("/org/bluez/hci0", False, RuntimeError("Timed out after applying"))
        code, _, error = self.cli(["--adapter", "hci2"])
        self.assertEqual(code, 1)
        self.assertIn("restored", error)
        self.assertEqual(self.bus.power, {"/org/bluez/hci0": True, "/org/bluez/hci2": False})
        self.assertEqual(self.bus.writes[-2:], [("/org/bluez/hci0", True), ("/org/bluez/hci2", False)])

    def test_successful_bus_reply_without_power_change_is_not_success(self):
        self.bus.ignore_once = ("/org/bluez/hci2", True)
        code, output, error = self.cli(["--adapter", "hci2"])
        self.assertEqual(code, 1)
        self.assertIn("did not become ON", error)
        self.assertNotIn("Selected:", output)
        self.assertTrue(self.bus.power["/org/bluez/hci0"])

    def test_ctrl_c_during_switch_restores_power(self):
        self.bus.fail_once = ("/org/bluez/hci0", False, KeyboardInterrupt())
        code, _, error = self.cli(["--adapter", "hci2"])
        self.assertEqual(code, 130)
        self.assertIn("restored", error)
        self.assertEqual(self.bus.power, {"/org/bluez/hci0": True, "/org/bluez/hci2": False})

    def test_changed_hci_identity_does_not_target_a_different_adapter(self):
        self.bus.break_identity = "/org/bluez/hci2"
        code, _, error = self.cli(["--adapter", "hci2"])
        self.assertEqual(code, 1)
        self.assertIn("changed identity", error)
        self.assertEqual(self.bus.writes, [])

    def test_no_ready_adapter_is_an_error_even_when_bluez_is_running(self):
        self.bus.addresses.clear()
        code, _, error = self.cli(["--list"])
        self.assertEqual(code, 1)
        self.assertIn("No ready BlueZ adapters", error)
        self.assertEqual(self.bus.writes, [])

    def test_permission_failure_explains_sudo_and_keeps_original_state(self):
        self.bus.fail_once = ("/org/bluez/hci2", True, RuntimeError("Access denied"))
        code, _, error = self.cli(["--adapter", "hci2"])
        self.assertEqual(code, 1)
        self.assertIn("sudo", error)
        self.assertTrue(self.bus.power["/org/bluez/hci0"])

    def test_inventory_failure_or_command_timeout_never_changes_devices(self):
        for failure in (RuntimeError("busctl timed out after 2s."), RuntimeError("Bluetooth service unavailable")):
            with self.subTest(error=failure), patch.object(bt, "run_command", side_effect=failure):
                code, _, error = self.cli(["--list"])
                self.assertEqual(code, 1)
                self.assertIn(str(failure), error)
                self.assertEqual(self.bus.writes, [])

    def test_rollback_failure_is_reported(self):
        self.bus.fail_once = ("/org/bluez/hci0", False, RuntimeError("Switch failed"))
        original_command = self.bus.command

        def command(args, timeout):
            if "set-property" in args and args[-1] == "true" and "/org/bluez/hci0" in args:
                raise RuntimeError("Adapter disappeared")
            return original_command(args, timeout)

        with patch.object(bt, "run_command", side_effect=command):
            code, _, error = self.cli(["--adapter", "hci2"])
        self.assertEqual(code, 1)
        self.assertIn("Could not restore every adapter", error)
        self.assertIn("Adapter disappeared", error)
        self.assertFalse(self.bus.power["/org/bluez/hci2"])


class HardwareNamesTest(unittest.TestCase):
    def test_usb_model_is_read_from_parent_without_sysfs_mac_attribute(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            usb = root / "devices/1-8"
            hci = usb / "1-8:1.0/bluetooth/hci7"
            hci.mkdir(parents=True)
            for name, value in {"idVendor": "1234", "idProduct": "abcd", "manufacturer": "Example", "product": "USB Radio"}.items():
                (usb / name).write_text(value)
            sysfs = root / "class/bluetooth"
            sysfs.mkdir(parents=True)
            (sysfs / "hci7").symlink_to(hci)
            with patch.object(bt, "run_command", side_effect=RuntimeError("udevadm unavailable")):
                model, usb_id = bt.hardware_name("hci7", "PC hostname", 1, sysfs)
            self.assertEqual((model, usb_id), ("Example USB Radio", "1234:abcd"))
            self.assertFalse((hci / "address").exists())

    def test_udev_database_names_are_available_for_non_usb_adapters(self):
        with tempfile.TemporaryDirectory() as directory:
            sysfs = Path(directory)
            (sysfs / "hci0").mkdir()
            udev = "ID_VENDOR_FROM_DATABASE=Example vendor\nID_MODEL_FROM_DATABASE=UART Bluetooth\n"
            with patch.object(bt, "run_command", return_value=udev):
                self.assertEqual(bt.hardware_name("hci0", "PC hostname", 1, sysfs),
                                 ("Example vendor UART Bluetooth", ""))

    def test_missing_sysfs_uses_bluez_name_and_mac_remains_available(self):
        with tempfile.TemporaryDirectory() as directory:
            self.assertEqual(bt.hardware_name("hci3", "PC hostname", 1, Path(directory)), ("PC hostname", ""))

    def test_hardware_text_cannot_inject_terminal_control_characters(self):
        value = bt.clean("USB\x1b[2J\nRadio\r\x07\u202emodel")
        self.assertFalse(any(ord(c) < 32 or c == "\u202e" for c in value))


class BusctlProtocolTest(unittest.TestCase):
    def test_nonzero_exit_includes_status_and_operation_even_without_output(self):
        command=["busctl","--system","call","org.bluez","/","org.freedesktop.DBus.ObjectManager","GetManagedObjects"]
        for code in (1,-9):
            result=subprocess.CompletedProcess(command,code,stdout='',stderr='')
            with self.subTest(code=code),patch.object(bt.subprocess,'run',return_value=result):
                with self.assertRaisesRegex(RuntimeError, f"code {code}: no diagnostic output.*GetManagedObjects"):
                    bt.run_command(command,2)
        result=subprocess.CompletedProcess(command,1,stdout='Useful diagnostic',stderr='\n')
        with patch.object(bt.subprocess,'run',return_value=result),self.assertRaisesRegex(RuntimeError,'Useful diagnostic'):
            bt.run_command(command,2)

    def test_scalar_and_single_element_properties_and_invalid_values(self):
        for value in (True, [True]):
            self.assertIs(bt.property_value(json.dumps({"type": "b", "data": value}), "b"), True)
        for invalid in ("not json", "[]", "null", '{"type":"b","data":"false"}', '{"type":"b","data":[true,false]}'):
            with self.subTest(invalid=invalid), self.assertRaisesRegex(RuntimeError, "Invalid property"):
                bt.property_value(invalid, "b")

    def test_command_deadline_kills_a_stuck_external_command(self):
        with self.assertRaisesRegex(RuntimeError, "timed out"):
            bt.run_command([sys.executable, "-c", "import time; time.sleep(20)"], 0.05)

    def test_invalid_deadlines_are_rejected(self):
        for invalid in ("0", "-1", "nan", "inf", "61", "wrong"):
            with self.subTest(value=invalid), self.assertRaises(argparse.ArgumentTypeError):
                bt.timeout_value(invalid)


if __name__ == "__main__":
    unittest.main()
