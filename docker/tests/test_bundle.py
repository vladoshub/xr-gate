import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tarfile
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import xr_gate


class BundleTest(unittest.TestCase):
    def test_config_is_literal_and_shell_environment_takes_precedence(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            (root / ".env.example").write_text('XR_VERSION=v1.2.3\n')
            (root / ".env").write_text('XR_TCP_PORT=45811\nXR_CONFIG_DIR="/tmp/config with spaces"\n')
            with patch.object(xr_gate, "ROOT", root), patch.dict(os.environ, {"XR_TCP_PORT": "45812"}, clear=True):
                settings = xr_gate.settings()
            self.assertEqual(settings["XR_TCP_PORT"], "45812")
            self.assertEqual(settings["XR_CONFIG_DIR"], "/tmp/config with spaces")
            (root / ".env").write_text('XR_CONFIG_DIR=$(touch /tmp/should-never-exist)\n')
            with self.assertRaises(ValueError):
                xr_gate.read_env(root / ".env")

    def test_release_selection_controls_export_and_image_check(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            (root / ".env.example").write_text('XR_VERSION=v1.2.3\n')
            (root / ".env").write_text('XR_VERSION=v1.2.4\n')
            with patch.object(xr_gate, "ROOT", root), patch.dict(os.environ, {}, clear=True):
                env = xr_gate.settings()
            self.assertEqual(xr_gate.image(env), "xr-gate-runtime:v1.2.4")
            with patch.object(xr_gate, "call", return_value="temporary-container") as run:
                xr_gate.export_configs(env, root / "exported")
            self.assertIn("xr-gate-runtime:v1.2.4", run.call_args_list[0].args[0])
            with patch.object(xr_gate, "ROOT", root), patch.dict(os.environ, {"XR_VERSION": "v1.2.5"}, clear=True), \
                 patch.object(sys, "argv", ["xr-gate", "check-image"]), \
                 patch.object(xr_gate, "local_engine"), patch.object(xr_gate, "call", return_value=0) as run:
                self.assertEqual(xr_gate.main(), 0)
            self.assertIn("xr-gate-runtime:v1.2.5", run.call_args.args[0])
            for invalid in ("", "latest/other", "../escape", "v1:tag", "v1 tag"):
                with self.subTest(invalid=invalid), self.assertRaises(ValueError):
                    xr_gate.image({"XR_VERSION": invalid})

    def test_hardware_file_and_user_overrides_are_independent_of_vendor(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            (root / ".env.example").write_text("XR_VERSION=v1.2.3\nXR_HARDWARE_ENV=custom.env\n")
            (root / "custom.env").write_text("XR_PROFILE=other_camera\nXR_USB_IDS=1234:abcd\nXR_CAMERA_TARGET=/dev/video4\n")
            (root / ".env").write_text("XR_USB_IDS=5678:abcd\n")
            with patch.object(xr_gate, "ROOT", root), patch.dict(os.environ, {}, clear=True):
                settings = xr_gate.settings()
                self.assertEqual(settings["XR_PROFILE"], "other_camera")
                self.assertEqual(xr_gate.selected_ids(settings), {"5678:abcd"})
                self.assertEqual(xr_gate.camera_target(settings, 1), "/dev/video5")
            with patch.object(xr_gate, "ROOT", root), patch.dict(os.environ, {"XR_HARDWARE_ENV": "missing.env"}, clear=True):
                with self.assertRaisesRegex(ValueError, "Hardware env not found"):
                    xr_gate.settings()

    def test_camera_mapping_preserves_release_video0_without_exposing_other_usb(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            sysroot, devroot = root / "sys", root / "dev"
            def device(name, vendor, product, bus, number):
                usb = sysroot / "devices" / name
                usb.mkdir(parents=True)
                for key, value in {"idVendor": vendor, "idProduct": product, "busnum": bus, "devnum": number}.items():
                    (usb / key).write_text(value)
                link = sysroot / "bus/usb/devices" / name
                link.parent.mkdir(parents=True, exist_ok=True)
                link.symlink_to(usb)
                return usb
            xreal = device("1-2", "3318", "0426", "1", "7")
            other = device("1-3", "1234", "5678", "1", "8")
            for owner, category, name, index in ((xreal, "video4linux", "video8", "0"),
                                                  (xreal, "video4linux", "video9", "1"),
                                                  (other, "video4linux", "video0", "0"),
                                                  (xreal, "hidraw", "hidraw3", "0")):
                actual = owner / category / name
                actual.mkdir(parents=True)
                (actual / "index").write_text(index)
                link = sysroot / "class" / category / name
                link.parent.mkdir(parents=True, exist_ok=True)
                link.symlink_to(actual)
            mapped = xr_gate.discover({"XR_USB_IDS": "3318:0426"}, sysroot, devroot)
            self.assertIn((str(devroot / "video8"), "/dev/video0"), mapped)
            self.assertIn((str(devroot / "hidraw3"), "/dev/hidraw3"), mapped)
            self.assertIn((str(devroot / "bus/usb/001/007"), str(devroot / "bus/usb/001/007")), mapped)
            self.assertFalse(any("/008" in source or source == str(devroot / "video0") for source, _ in mapped))
            selected = xr_gate.discover({"XR_USB_IDS": "3318:0426", "XR_CAMERA_DEVICE": str(devroot / "video9")}, sysroot, devroot)
            self.assertEqual(selected[0], (str(devroot / "video9"), "/dev/video0"))
            explicit = xr_gate.discover({"XR_CAMERA_DEVICE": str(devroot / "video0")}, sysroot, devroot)
            self.assertEqual(explicit, [(str(devroot / "video0"), "/dev/video0")])
            self.assertEqual(xr_gate.discover({}, sysroot, devroot), [])

    def test_absent_usb_is_an_error_before_container_creation(self):
        with patch.object(xr_gate, "discover", return_value=[]):
            with self.assertRaisesRegex(RuntimeError, "No selected UVC"):
                xr_gate.hardware_override({})

    def test_camera_only_and_serial_imu_profiles_do_not_require_hidraw(self):
        devices = [("/dev/null", "/dev/video0"), ("/dev/zero", "/dev/ttyACM0")]
        with patch.object(xr_gate, "discover", return_value=devices):
            runtime = xr_gate.hardware_override({})["services"]["runtime"]
            self.assertEqual(runtime["devices"], ["/dev/null:/dev/video0:rw", "/dev/zero:/dev/ttyACM0:rw"])
            with self.assertRaisesRegex(RuntimeError, "No selected hidraw"):
                xr_gate.hardware_override({"XR_REQUIRE_HIDRAW": "1"})

    def test_remote_profile_can_run_without_local_usb_devices(self):
        with patch.object(xr_gate, "discover", return_value=[]):
            runtime = xr_gate.hardware_override({"XR_REQUIRE_CAMERA": "0"})["services"]["runtime"]
            self.assertNotIn("devices", runtime)

    def test_serial_discovery_follows_selected_usb_identity_and_explicit_nodes(self):
        with tempfile.TemporaryDirectory() as folder:
            sysroot, devroot = Path(folder) / "sys", Path(folder) / "dev"
            for name, vendor in (("ttyACM0", "1234"), ("ttyUSB1", "5678")):
                usb = sysroot / "devices" / name
                usb.mkdir(parents=True)
                (usb / "idVendor").write_text(vendor)
                (usb / "idProduct").write_text("abcd")
                actual = usb / "tty" / name
                actual.mkdir(parents=True)
                link = sysroot / "class/tty" / name
                link.parent.mkdir(parents=True, exist_ok=True)
                link.symlink_to(actual)
            mapped = xr_gate.discover({"XR_USB_IDS": "1234:abcd", "XR_SERIAL_DEVICES": "/dev/serial/by-id/custom-imu"}, sysroot, devroot)
            self.assertEqual(mapped, [(str(devroot / "ttyACM0"), "/dev/ttyACM0"),
                                      ("/dev/serial/by-id/custom-imu", "/dev/serial/by-id/custom-imu")])
            for invalid in ("/dev", "/dev/serial/by-id/..", "/dev/serial/by-id/a/b", "/etc/passwd"):
                with self.subTest(invalid=invalid), self.assertRaises(ValueError):
                    xr_gate.discover({"XR_SERIAL_DEVICES": invalid}, sysroot, devroot)

    def test_device_defaults_are_loaded_from_the_environment_profile(self):
        with patch.dict(os.environ, {}, clear=True):
            env = xr_gate.settings()
        self.assertEqual(xr_gate.selected_ids(env), {"3318:0426"})
        self.assertEqual(xr_gate.selected_ids({}), set())
        with patch.dict(os.environ, {"XR_USB_IDS": "1234:abcd", "XR_REQUIRE_HIDRAW": "0"}, clear=True):
            self.assertEqual(xr_gate.selected_ids(xr_gate.settings()), {"1234:abcd"})
        with patch.dict(os.environ, {"XR_REQUIRE_CAMERA": "yes"}, clear=True), self.assertRaises(ValueError):
            xr_gate.settings()

    def test_explicit_serial_nodes_still_have_to_be_character_devices(self):
        with patch.object(xr_gate, "discover", return_value=[("/etc/hosts", "/dev/ttyACM0")]):
            with self.assertRaisesRegex(RuntimeError, "not a character device"):
                xr_gate.hardware_override({"XR_REQUIRE_CAMERA": "0"})

    def test_evdev_requires_explicit_event_nodes(self):
        with self.assertRaises(ValueError):
            xr_gate.discover({"XR_INPUT_DEVICES": "/dev/input"}, Path("/nonexistent"), Path("/dev"))

    def test_old_or_remote_engine_is_rejected(self):
        with patch.object(xr_gate.shutil, "which", return_value="docker"):
            with self.assertRaisesRegex(RuntimeError, "local Linux"):
                xr_gate.local_engine({"DOCKER_HOST": "tcp://example.org:2375"})
            with patch.object(xr_gate, "call", return_value=json.dumps({"OSType": "linux", "ServerVersion": "27.5.1"})):
                with self.assertRaisesRegex(RuntimeError, ">= 28"):
                    xr_gate.local_engine({"DOCKER_HOST": "unix:///var/run/docker.sock"})

    def test_entrypoint_passes_stdin_tcp_env_and_config_without_editing_release(self):
        for override in (None, 'default', 'gearvr'):
            with self.subTest(override=override), tempfile.TemporaryDirectory() as folder:
                root = Path(folder)
                config, home = root / "config", root / "home"
                config.mkdir()
                home.mkdir()
                cpu = root / "devices/common/linux/scripts/runtime/cpu_target.py"
                cpu.parent.mkdir(parents=True)
                cpu.write_text("pass\n")
                launcher = root / "run_xr_client.sh"
                launcher.write_text('#!/usr/bin/env bash\npython3 -c \'import json,os,sys; print(json.dumps({"args":sys.argv[1:],"tcp":os.environ["RUNTIME_APP_TCP"],"bind":os.environ["RUNTIME_APP_TCP_HOST"],"input":sys.stdin.read()}))\' "$@"\n')
                launcher.chmod(0o755)
                if override:
                    (config / "xr_client.json").write_text(json.dumps({'test': True, 'post_gate_services': [
                        {'name': 'override_controller', 'env': {'CONFIG_PATH':
                            f'~/.config/xr_tracking/override_controller/{override}.json'}}]}))
                    (config / "override_controller.json").write_text('{"controller":true}\n')
                    state = home / f'.config/xr_tracking/override_controller/{override}.json'
                    state.parent.mkdir(parents=True)
                    state.write_text('{"previous":"save"}')
                env = dict(os.environ, XR_ROOT_PROJECT=str(root), XR_CONFIG_ROOT=str(config), XR_PROFILE="test_camera", HOME=str(home))
                output = subprocess.run(["bash", str(ROOT / "image/entrypoint.sh"), "--no-imu"], env=env,
                                        input="interactive input\n", text=True, capture_output=True, check=True)
                result = json.loads(output.stdout.splitlines()[-1])
                self.assertEqual(result["input"], "interactive input\n")
                self.assertEqual(result["tcp"], "1")
                self.assertEqual(result["bind"], "0.0.0.0")
                expected = str(config / "xr_client.json") if override else "test_camera"
                self.assertEqual(result["args"], ["--config", expected, "--no-imu"])
                if override:
                    self.assertEqual(state.read_bytes(), (config / "override_controller.json").read_bytes())
                    self.assertEqual(next(state.parent.glob(f'{override}.before-*.json')).read_text(), '{"previous":"save"}')
                    if override == 'gearvr':
                        self.assertFalse((state.parent / 'default.json').exists())

    def test_release_extractor_rejects_escape(self):
        spec = importlib.util.spec_from_file_location("release_installer", ROOT / "image/install-release.py")
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        with tempfile.TemporaryDirectory() as folder:
            archive = Path(folder) / "bad.tar.gz"
            with tarfile.open(archive, "w:gz") as writer:
                member = tarfile.TarInfo("../escape")
                writer.addfile(member)
            with self.assertRaisesRegex(RuntimeError, "Unsafe archive path"):
                module.extract_checked(archive, folder)


if __name__ == "__main__":
    unittest.main()
