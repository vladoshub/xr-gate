import importlib.util
import json
from pathlib import Path
import shutil
import os
import subprocess
import sys
from unittest.mock import patch
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
                for category, prefix in (("video4linux", "video"), ("hidraw", "hidraw"), ("tty", "ttyACM"), ("sound", "controlC")):
                    actual = usb / category / (prefix + number)
                    actual.mkdir(parents=True)
                    link = sysroot / "class" / category / actual.name
                    link.parent.mkdir(parents=True, exist_ok=True)
                    link.symlink_to(actual)
            nodes, cameras, hids = runtime.device_nodes(sysroot, devroot)
            self.assertEqual(cameras, [devroot / "video11"])
            self.assertEqual(hids, [devroot / "hidraw11"])
            self.assertIn(devroot / "ttyACM11", nodes)
            self.assertIn(devroot / "snd/controlC11", nodes)
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
    def check_documentation_and_images(self, package):
        names = package.namelist()
        prefix = packager.ROOT_NAME + "/"
        self.assertFalse(any(len(Path(name).parts) == 2 and Path(name).suffix in (".md", ".html") for name in names))
        for name in ("README.md", "README.ru.md", "START-HERE.html", "START-HERE.ru.html", "GESTURES.html", "GESTURES.ru.html", "IMAGE-ASSETS.md"):
            self.assertIn(prefix + "docs/" + name, names)
        for name in ("GESTURES.html", "GESTURES.ru.html"):
            self.assertIn(b"<svg", package.read(prefix + "docs/" + name))
        self.assertIn(b"MIT License", package.read(prefix + "docs/IMAGE-ASSETS.md"))
        self.assertEqual(package.read(prefix + "docs/licenses/fixture/NOTICE.txt"), b"fixture runtime notice")
        self.assertFalse(any(name.startswith(prefix + "licenses/") for name in names))
        for image in (WIN / "images").glob("*.png"):
            self.assertEqual(package.read(prefix + "images/" + image.name), image.read_bytes())

    def launcher(self, root):
        path = root / "XR-Gate.exe"
        path.write_bytes(b"MZ" + b"launcher test fixture" * 8)
        notice = root / "licenses/fixture/NOTICE.txt"
        notice.parent.mkdir(parents=True, exist_ok=True)
        notice.write_bytes(b"fixture runtime notice")
        return path

    def drivers(self, root, frequencies="60 90", modes="direct extended_sbs", device="xreal_ultra"):
        for hz, mode in packager.selections(frequencies, modes):
            package = root / device / f"{hz}Hz-{mode}" / "xr_tracking"
            for name in ("driver.vrdrivermanifest", "bin/win64/driver_xr_tracking.dll", "bin/win64/openvr_api.dll",
                         "LICENSE", "THIRD_PARTY_NOTICES.md", "Install-Driver.bat", "Uninstall-Driver.cmd"):
                path = package / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text("fixture")
            settings = package / "resources/settings/default.vrsettings"
            settings.parent.mkdir(parents=True)
            settings.write_text(json.dumps({"xr_tracking": {"deviceProfile": device, "modelNumber": device, "displayFrequency": hz,
                "isDisplayOnDesktop": mode == "extended_sbs", "runtimePoseTransport": "tcp", "runtimeHandTransport": "tcp",
                "runtimeControllerStateTransport": "tcp"}}))

    def test_one_archive_contains_all_variants_and_complete_installer_without_user_env(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            source = root / "win"
            shutil.copytree(WIN, source, ignore=shutil.ignore_patterns("__pycache__", "tests"))
            (source / ".env").write_text("PRIVATE=value")
            self.drivers(root / "drivers")
            archive = root / "xr-gate-windows-wsl2-usbip.zip"
            executable = self.launcher(root)
            packager.build(source, root / "drivers", archive, "60 90", "direct extended_sbs", "v9.9.9", launcher=executable)
            manifest = packager.verify(archive, "60 90", "direct extended_sbs")
            self.assertEqual(len(manifest["variants"]), 4)
            with ZipFile(archive) as package:
                self.check_documentation_and_images(package)
                self.assertNotIn("xr-gate-windows-wsl2-usbip/.env", package.namelist())
                self.assertFalse(any(name.lower().endswith((".cmd", ".bat")) for name in package.namelist()))
                self.assertFalse(any("/launcher/" in name or "/obj/" in name for name in package.namelist()))
                self.assertEqual(package.read("xr-gate-windows-wsl2-usbip/XR-Gate.exe"), executable.read_bytes())
                self.assertIn(b"XR_RELEASE_TAG=v9.9.9", package.read("xr-gate-windows-wsl2-usbip/.env.example"))
                self.assertEqual(package.read("xr-gate-windows-wsl2-usbip/linux/xr_gate_audio.py"), (WIN / "linux/xr_gate_audio.py").read_bytes())
                for hz, mode in packager.selections("60 90", "direct extended_sbs"):
                    relative = packager.variant_path(hz, mode, "xreal_ultra")
                    original = root / "drivers" / "xreal_ultra" / f"{hz}Hz-{mode}" / "xr_tracking"
                    for source_file in original.rglob("*"):
                        if source_file.is_file() and source_file.suffix.lower() not in (".bat", ".cmd"):
                            entry = f"{packager.ROOT_NAME}/{relative}/{source_file.relative_to(original).as_posix()}"
                            self.assertEqual(package.read(entry), source_file.read_bytes())
            self.assertTrue(archive.with_suffix(".zip.sha256").is_file())

    def test_single_selected_variant_sets_installable_defaults_and_missing_driver_fails(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            self.drivers(root / "drivers", "90", "direct")
            archive = root / "xr-gate-windows-wsl2-usbip.zip"
            executable = self.launcher(root)
            packager.build(WIN, root / "drivers", archive, "90", "direct", launcher=executable)
            with ZipFile(archive) as package:
                env = package.read("xr-gate-windows-wsl2-usbip/.env.example").decode()
                preset = package.read("xr-gate-windows-wsl2-usbip/" + packager.hardware_preset(env)).decode()
                self.assertIn("XR_DISPLAY_FREQUENCY_HZ=90", preset)
                self.assertIn("XR_DISPLAY_MODE=direct", preset)
            with self.assertRaises(ValueError):
                packager.verify(archive, "60 90", "direct")
            with self.assertRaises(FileNotFoundError):
                packager.build(WIN, root / "drivers", archive, "60 90", "direct", launcher=executable)

    def test_launcher_ci_archive_contains_support_scripts_and_can_download_drivers(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            executable = self.launcher(root)
            archive = root / "launcher.zip"
            packager.build_launcher(WIN, executable, archive)
            with ZipFile(archive) as package:
                self.assertNotIn("xr-gate-windows-wsl2-usbip/bundle.json", package.namelist())
                self.assertIn("xr-gate-windows-wsl2-usbip/windows/Driver.ps1", package.namelist())
                self.check_documentation_and_images(package)
                self.assertFalse(any(name.lower().endswith((".bat", ".cmd")) for name in package.namelist()))
                self.assertFalse(any("/launcher/" in name for name in package.namelist()))
                metadata = json.loads(package.read("xr-gate-windows-wsl2-usbip/launcher.json"))["launcher"]
                packager.verify_launcher_metadata(metadata, packager.verify_installer(package))
            executable.write_bytes(b"not a Windows executable")
            with self.assertRaises(ValueError):
                packager.build_launcher(WIN, executable, archive)


class ManagedStopTest(unittest.TestCase):
    def runtime(self, root):
        runtime = linux.Runtime.__new__(linux.Runtime)
        runtime.base = root
        runtime.current = root / "current"
        runtime.lock = root / "runtime.lock"
        runtime.user_required = lambda: None
        return runtime

    def spawn_locked(self, root):
        script = "import fcntl,sys,time; f=open(sys.argv[1],'w'); fcntl.flock(f,fcntl.LOCK_EX); print('ready',flush=True); time.sleep(60)"
        child = subprocess.Popen([sys.executable, "-c", script, str(root / "runtime.lock")], stdout=subprocess.PIPE, text=True)
        self.addCleanup(self.cleanup_child, child)
        self.assertEqual(child.stdout.readline().strip(), "ready")
        return child

    @staticmethod
    def cleanup_child(child):
        if child.poll() is None:
            child.terminate()
        child.wait(timeout=5)
        if child.stdout:
            child.stdout.close()

    def test_stop_signals_only_the_verified_managed_process(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            runtime = self.runtime(root)
            child = self.spawn_locked(root)
            unrelated = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(60)"])
            self.addCleanup(self.cleanup_child, unrelated)
            session = root / "runtime-session.json"
            identity = {"pid": child.pid, "uid": os.geteuid(), "start_ticks": "12345", "boot_id": "test-boot"}
            session.write_text(json.dumps(identity))
            # Some container runners virtualize /proc independently of PID FDs.
            # Stub the identity reader, while using a real lock and pidfd signal.
            with patch.object(linux, "process_identity", return_value=identity):
                runtime.stop()
            child.wait(timeout=5)
            self.assertIsNone(unrelated.poll())
            self.assertFalse(session.exists())
            self.assertFalse(runtime.status()["running"])

    def test_reused_or_stale_pid_is_never_signalled(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            child = self.spawn_locked(root)
            identity = {"pid": child.pid, "uid": os.geteuid(), "start_ticks": "12346", "boot_id": "test-boot"}
            (root / "runtime-session.json").write_text(json.dumps(identity))
            current = dict(identity, start_ticks="12345")
            with patch.object(linux, "process_identity", return_value=current):
                with self.assertRaisesRegex(RuntimeError, "different process"):
                    self.runtime(root).stop()
            self.assertIsNone(child.poll())

    def test_legacy_runtime_requires_console_shutdown(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            child = self.spawn_locked(root)
            with self.assertRaisesRegex(RuntimeError, "older launcher"):
                self.runtime(root).stop()
            self.assertIsNone(child.poll())

    def test_identity_reader_handles_process_names_and_records_boot_and_start_time(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            process = root / "proc/42"
            process.mkdir(parents=True)
            # proc stat field 22 is starttime; names may themselves contain ')'.
            fields = ["S"] + ["0"] * 18 + ["987654"] + ["0"] * 8
            (process / "stat").write_text("42 (name with ) parens) " + " ".join(fields))
            (root / "proc/self").mkdir()
            (root / "proc/self/stat").write_text(str(os.getpid()) + " (fixture) " + " ".join(fields))
            boot = root / "proc/sys/kernel/random/boot_id"
            boot.parent.mkdir(parents=True)
            boot.write_text("test-boot\n")
            with patch.object(linux, "Path", side_effect=lambda value: root / str(value).lstrip("/")):
                identity = linux.process_identity(42)
            self.assertEqual(identity, {"pid": 42, "start_ticks": "987654", "boot_id": "test-boot", "uid": os.geteuid()})


if __name__ == "__main__":
    unittest.main()
