import importlib.util
import json
from pathlib import Path
import shutil
import tempfile
import unittest
from zipfile import ZipFile

WIN = Path(__file__).resolve().parents[1]


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


packager = module("windows_packager", WIN / "package_windows.py")
linux = module("windows_linux_helper", WIN / "linux/xr_gate_wsl.py")


class HardwareTest(unittest.TestCase):
    def runtime(self, root, settings):
        runtime = linux.Runtime.__new__(linux.Runtime)
        runtime.cfg = {"settings": settings}
        runtime.settings = settings
        runtime.base = root
        runtime.current = root / "current"
        runtime.config = root / "config"
        runtime.current.mkdir()
        runtime.config.mkdir()
        def write(path, text):
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text)
        runtime.write = write
        return runtime

    def test_generic_usb_selection_excludes_unrelated_camera_and_hid(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            runtime = self.runtime(root, {"XR_USB_IDS": "1234:abcd"})
            sysroot, devroot = root / "sys", root / "dev"
            for name, vendor, number in (("selected", "1234", "11"), ("other", "3318", "12")):
                usb = sysroot / "devices" / name
                usb.mkdir(parents=True)
                (usb / "idVendor").write_text(vendor)
                (usb / "idProduct").write_text("abcd")
                for category, prefix in (("video4linux", "video"), ("hidraw", "hidraw"), ("tty", "ttyACM")):
                    actual = usb / category / (prefix + number)
                    actual.mkdir(parents=True)
                    link = sysroot / "class" / category / actual.name
                    link.parent.mkdir(parents=True, exist_ok=True)
                    link.symlink_to(actual)
            nodes, cameras, hids = runtime.device_nodes(sysroot, devroot)
            self.assertEqual(cameras, [devroot / "video11"])
            self.assertEqual(hids, [devroot / "hidraw11"])
            self.assertIn(devroot / "ttyACM11", nodes)
            self.assertFalse(any("12" in str(node) for node in nodes))
            runtime.settings = {"XR_USB_IDS": "", "XR_CAMERA_DEVICE": str(devroot / "video12")}
            self.assertEqual(runtime.device_nodes(sysroot, devroot)[1], [devroot / "video12"])

    def test_profile_and_controller_provider_come_from_settings(self):
        with tempfile.TemporaryDirectory() as folder:
            runtime = self.runtime(Path(folder), {"XR_PROFILE": "custom_camera", "XR_CONTROLLER_PROVIDERS": "custom_input"})
            profile = runtime.current / "bin/python/xr_client/configs/custom_camera.json"
            profile.parent.mkdir(parents=True)
            profile.write_text(json.dumps({"post_gate_services": [{"name": "override_controller", "env": {}}]}))
            generated = runtime.generate_profile()
            data = json.loads(generated.read_text())
            self.assertEqual(generated.name, "profile.json")
            self.assertEqual(data["post_gate_services"][0]["env"]["PROVIDERS"], "custom_input")
            custom = runtime.config / "xr_client.json"
            custom.write_text('{"custom":true}')
            self.assertEqual(runtime.generate_profile(), custom)

    def test_custom_display_geometry_and_tcp_are_applied_to_both_settings_files(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            driver, steam = root / "driver.json", root / "steam.json"
            driver.write_text('{"keep":true,"xr_tracking":{"modelNumber":"XREAL Air 2 Ultra"}}')
            steam.write_text('{"steamvr":{"debugMode":true,"DebugMode":true}}')
            request = {"window_x": -2520, "window_y": 120, "frequency_hz": 72,
                       "display_mode": "extended_sbs", "driver_settings": str(driver), "steamvr_settings": str(steam),
                       "settings": {"XR_WINDOW_WIDTH": "2520", "XR_WINDOW_HEIGHT": "1440", "XR_RENDER_WIDTH": "1260",
                                    "XR_RENDER_HEIGHT": "1440", "XR_TCP_PORT": "45910", "XR_TCP_HOST": "127.0.0.1"}}
            linux.configure_windows_display(request)
            for file in (driver, steam):
                data = json.loads(file.read_text())
                self.assertEqual(data["xr_tracking"]["windowWidth"], 2520)
                self.assertEqual(data["xr_tracking"]["renderWidth"], 1260)
                self.assertEqual(data["xr_tracking"]["displayFrequency"], 72)
                if file == driver:
                    self.assertEqual(data["xr_tracking"]["modelNumber"], "XREAL Air 2 Ultra")
                else:
                    self.assertNotIn("modelNumber", data["xr_tracking"])
                self.assertEqual(data["xr_tracking"]["runtimeTcpPort"], 45910)
                self.assertFalse(data["steamvr"]["directMode"])
            self.assertTrue(json.loads(driver.read_text())["keep"])
            before = driver.read_bytes(), steam.read_bytes()
            request["settings"]["XR_WINDOW_WIDTH"] = "0"
            with self.assertRaises(ValueError):
                linux.configure_windows_display(request)
            self.assertEqual(before, (driver.read_bytes(), steam.read_bytes()))


class PackageTest(unittest.TestCase):
    def drivers(self, root, frequencies="60 90", modes="direct extended_sbs"):
        for hz, mode in packager.selections(frequencies, modes):
            package = root / f"{hz}Hz-{mode}" / "xr_tracking"
            for name in ("driver.vrdrivermanifest", "bin/win64/driver_xr_tracking.dll", "bin/win64/openvr_api.dll",
                         "LICENSE", "THIRD_PARTY_NOTICES.md"):
                path = package / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text("fixture")
            settings = package / "resources/settings/default.vrsettings"
            settings.parent.mkdir(parents=True)
            settings.write_text(json.dumps({"xr_tracking": {"modelNumber": "XREAL Air 2 Ultra", "displayFrequency": hz,
                "isDisplayOnDesktop": mode == "extended_sbs", "runtimePoseTransport": "tcp", "runtimeHandTransport": "tcp",
                "runtimeControllerStateTransport": "tcp"}}))

    def test_one_archive_contains_all_variants_and_complete_installer_without_user_env(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            source = root / "win"
            shutil.copytree(WIN, source, ignore=shutil.ignore_patterns("__pycache__", "tests"))
            (source / ".env").write_text("PRIVATE=value")
            self.drivers(root / "drivers")
            archive = root / "xr-gate-windows.zip"
            packager.build(source, root / "drivers", archive, "60 90", "direct extended_sbs", "v9.9.9")
            manifest = packager.verify(archive, "60 90", "direct extended_sbs")
            self.assertEqual(len(manifest["variants"]), 4)
            with ZipFile(archive) as package:
                self.assertNotIn("xr-gate-windows/.env", package.namelist())
                self.assertIn(b"XR_RELEASE_TAG=v9.9.9", package.read("xr-gate-windows/.env.example"))
                for hz, mode in packager.selections("60 90", "direct extended_sbs"):
                    relative = packager.variant_path(hz, mode)
                    original = root / "drivers" / f"{hz}Hz-{mode}" / "xr_tracking"
                    for source_file in original.rglob("*"):
                        if source_file.is_file():
                            entry = f"{packager.ROOT_NAME}/{relative}/{source_file.relative_to(original).as_posix()}"
                            self.assertEqual(package.read(entry), source_file.read_bytes())
            self.assertTrue(archive.with_suffix(".zip.sha256").is_file())

    def test_single_selected_variant_sets_installable_defaults_and_missing_driver_fails(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            self.drivers(root / "drivers", "90", "direct")
            archive = root / "xr-gate-windows.zip"
            packager.build(WIN, root / "drivers", archive, "90", "direct")
            with ZipFile(archive) as package:
                env = package.read("xr-gate-windows/.env.example").decode()
                preset = package.read("xr-gate-windows/" + packager.hardware_preset(env)).decode()
                self.assertIn("XR_DISPLAY_FREQUENCY_HZ=90", preset)
                self.assertIn("XR_DISPLAY_MODE=direct", preset)
            with self.assertRaises(ValueError):
                packager.verify(archive, "60 90", "direct")
            with self.assertRaises(FileNotFoundError):
                packager.build(WIN, root / "drivers", archive, "60 90", "direct")


if __name__ == "__main__":
    unittest.main()
