import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

SPEC = importlib.util.spec_from_file_location("controller_modes", Path(__file__).resolve().parents[1] / "linux/xr_gate_wsl.py")
linux = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(linux)


class ControllerModesTest(unittest.TestCase):
    def fixture(self, root, mode, custom=False):
        runtime = linux.Runtime.__new__(linux.Runtime)
        runtime.base = root
        runtime.config = root / "config"
        runtime.current = root / "current"
        runtime.config.mkdir()
        runtime.current.mkdir()
        runtime.settings = {"XR_PROFILE": "fixture", "XR_CONTROLLER_MODE": mode,
                            "XR_GEARVR_PROVIDERS": "evdev,gearvr_ble", "XR_GEARVR_TEMPLATE": "fixture.json"}
        runtime.cfg = {"gearvr": mode == "gearvr", "settings": runtime.settings}
        def write(path, text):
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text)
        runtime.write = write
        template = runtime.current / "bin/override_controller/configs/fixture.json"
        write(template, '{"template":true}')
        original = {
            "pre_gate_services": [{"name": "capture_service"}, {"name": "override_controller_service"}],
            "post_gate_services": [{"name": "override_controller", "enabled": False, "start_on_launch": False,
                                    "env": {"KEEP": "yes"}}, {"name": "hand_tracking"}, {"name": "xr_runtime_adapter"}],
            "foreground_services": [], "custom_value": "keep"
        }
        source = runtime.config / "xr_client.json" if custom else runtime.current / "bin/python/xr_client/configs/fixture.json"
        write(source, json.dumps(original))
        write(runtime.config / "controllers-ready", "registered")
        return runtime, source

    def test_gestures_never_contains_a_publisher_even_with_a_paired_ready_marker_or_custom_profile(self):
        for custom in (False, True):
            with self.subTest(custom=custom), tempfile.TemporaryDirectory() as folder:
                runtime, source = self.fixture(Path(folder), "gestures", custom)
                before = source.read_bytes()
                data = json.loads(runtime.generate_profile().read_text())
                names = [service["name"] for group in ("pre_gate_services", "post_gate_services", "foreground_services") for service in data[group]]
                self.assertNotIn("override_controller", names)
                self.assertNotIn("override_controller_service", names)
                self.assertEqual(set(names), {"capture_service", "hand_tracking", "xr_runtime_adapter"})
                self.assertEqual(data["custom_value"], "keep")
                self.assertEqual(source.read_bytes(), before)

    def test_gearvr_starts_publisher_and_keeps_existing_mac_mapping(self):
        for custom in (False, True):
            with self.subTest(custom=custom), tempfile.TemporaryDirectory() as folder:
                runtime, source = self.fixture(Path(folder), "gearvr", custom)
                (runtime.config / "controllers-ready").unlink()
                config = runtime.config / "override_controller.json"
                config.write_text('{"mac":"AA:BB:CC:DD:EE:FF","mapping":"learned"}')
                before = config.read_bytes(), source.read_bytes()
                data = json.loads(runtime.generate_profile().read_text())
                service = data["post_gate_services"][0]
                self.assertTrue(service["enabled"])
                self.assertTrue(service["start_on_launch"])
                self.assertEqual(service["enable_env"], "XR_GATE_CONTROLLER_INPUT")
                self.assertEqual(service["env"]["PROVIDERS"], "evdev,gearvr_ble")
                self.assertEqual(service["env"]["KEEP"], "yes")
                self.assertEqual((config.read_bytes(), source.read_bytes()), before)
                runtime.settings["XR_CONTROLLER_MODE"] = "gestures"
                self.assertFalse(any(service["name"] == "override_controller" for service in json.loads(runtime.generate_profile().read_text())["post_gate_services"]))

    def test_gearvr_bootstraps_only_the_configured_template(self):
        with tempfile.TemporaryDirectory() as folder:
            runtime, _ = self.fixture(Path(folder), "gearvr")
            runtime.generate_profile()
            self.assertEqual(json.loads((runtime.config / "override_controller.json").read_text()), {"template": True})

    def test_unknown_mode_and_profile_without_a_publisher_fail_explicitly(self):
        with tempfile.TemporaryDirectory() as folder:
            runtime, source = self.fixture(Path(folder), "invalid")
            with self.assertRaisesRegex(ValueError, "XR_CONTROLLER_MODE"):
                runtime.generate_profile()
            runtime.settings["XR_CONTROLLER_MODE"] = "gearvr"
            source.write_text('{"post_gate_services":[]}')
            with self.assertRaisesRegex(ValueError, "no override_controller"):
                runtime.generate_profile()

    def test_partial_custom_gestures_profile_keeps_unmentioned_default_sections(self):
        with tempfile.TemporaryDirectory() as folder:
            runtime, source = self.fixture(Path(folder), "gestures", custom=True)
            source.write_text('{"custom_value":"keep"}')
            data = json.loads(runtime.generate_profile().read_text())
            self.assertNotIn("pre_gate_services", data)
            self.assertNotIn("post_gate_services", data)
            self.assertNotIn("foreground_services", data)
            # The runtime environment must also suppress the publishers which
            # xr_client supplies as defaults for these omitted sections.
            runtime.settings["XR_TCP_PORT"] = "45810"
            runtime.run_managed = lambda args, env, handle: self.assertEqual(
                (env["RUN_OVERRIDE_CONTROLLER"], env["RUN_OVERRIDE_CONTROLLER_SERVICE"]), ("0", "0"))
            with (Path(folder) / "lock").open("w") as handle, patch.object(linux.socket, "socket"), patch.object(linux.os, "chdir"):
                runtime._start_locked(handle)


class StartupCancellationTest(unittest.TestCase):
    def test_startup_guard_accepts_only_the_owned_uncancelled_token(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "window.json"
            request = {"runtime_guard": {"path": str(path), "token": "a" * 32}}
            path.write_text(json.dumps({"Token": "a" * 32, "Stopping": False}))
            linux.check_runtime_guard(request)
            for record in ({"Token": "b" * 32, "Stopping": False}, {"Token": "a" * 32, "Stopping": True}):
                path.write_text(json.dumps(record))
                with self.assertRaises(RuntimeError):
                    linux.check_runtime_guard(request)
            linux.check_runtime_guard({})  # Existing CLI sessions remain supported.

    def test_cancelled_late_launch_closes_the_lock_before_any_runtime_or_usb_access(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "window.json"
            path.write_text(json.dumps({"Token": "a" * 32, "Stopping": True}))
            runtime = linux.Runtime.__new__(linux.Runtime)
            runtime.cfg = {"runtime_guard": {"path": str(path), "token": "a" * 32}}
            runtime.user_required = lambda: None
            runtime.status = lambda: {"installed": True}
            handle = (Path(folder) / "lock").open("w")
            runtime.acquire_lock = lambda: handle
            with self.assertRaisesRegex(RuntimeError, "cancelled"):
                runtime.start()
            self.assertTrue(handle.closed)


if __name__ == "__main__":
    unittest.main()
