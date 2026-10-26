import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
LAUNCHER = ROOT / "drivers/steam_vr/scripts/linux/start_openvr.py"
REGISTRAR = ROOT / "drivers/openvr_driver/scripts/register_driver.sh"


class LauncherTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.home = self.root / "home"
        self.home.mkdir()
        self.package = self.root / "relocated package"
        target = self.package / "bin/scripts/drivers/openvr_driver/register_driver.sh"
        target.parent.mkdir(parents=True)
        shutil.copy2(REGISTRAR, target)
        self.commands = self.root / "commands"
        self.commands.mkdir()
        self.stub("pgrep", "exit 1\n")
        self.stub("xrandr", 'echo "unexpected xrandr" >&2; exit 99\n')
        self.stub("steam", "exec python3 -c 'import json,os; print(\"ENV=\"+json.dumps(dict(os.environ)))'\n")
        self.env = {k: v for k, v in os.environ.items()
                    if not k.startswith(("XR_", "VK_", "__GL", "__NV", "WAYLAND", "XDG_"))
                    and k not in ("DRIVER_PACKAGE", "DRIVERS_ROOT", "ROOT_PROJECT", "DRI_PRIME")}
        self.env.update(HOME=str(self.home), PATH=str(self.commands) + os.pathsep + os.environ["PATH"],
                        XR_PACKAGE_ROOT=str(self.package), XR_OPENVR_DISPLAY_MODE="extended_sbs",
                        XR_DISPLAY_FREQUENCY_HZ="60", XR_OPENVR_LAUNCH_MODE="none", XDG_SESSION_TYPE="x11")
        self.config = self.home / ".local/share/Steam/config/steamvr.vrsettings"

    def stub(self, name, text):
        path = self.commands / name
        path.write_text("#!/bin/sh\n" + text)
        path.chmod(0o755)

    def driver(self, profile="alpha", hz=60, extended=False, legacy=False):
        name = f"openvr_driver_{hz}HZ" + ("_extended_sbs" if extended else "")
        path = self.package / "bin/drivers"
        if not legacy:
            path /= profile
        path = path / name / "xr_tracking"
        (path / "resources/settings").mkdir(parents=True)
        (path / "driver.vrdrivermanifest").write_text('{}')
        xr = dict(displayFrequency=hz, isDisplayOnDesktop=extended, windowX=0, windowY=0,
                  windowWidth=3840, windowHeight=1080, runtimePoseTransport="posix_shm")
        if profile:
            xr["deviceProfile"] = profile
        (path / "resources/settings/default.vrsettings").write_text(json.dumps({"xr_tracking": xr}))
        return path

    def run_launcher(self, **env):
        return subprocess.run([sys.executable, str(LAUNCHER)], env=dict(self.env, **env),
                              text=True, capture_output=True, timeout=20)

    def success(self, **env):
        result = self.run_launcher(**env)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return result

    def test_minimal_command_uses_same_frequency_driver_and_creates_settings(self):
        driver = self.driver()
        self.success()
        config = json.loads(self.config.read_text())
        self.assertTrue(config["xr_tracking"]["isDisplayOnDesktop"])
        self.assertTrue(config["steamvr"]["allowFallbackMirrorWindowLinux"])
        self.assertFalse(config["direct_mode"]["enable"])
        self.assertEqual(config["xr_tracking"]["displayFrequency"], 60)
        registry = json.loads((self.home / ".config/openvr/openvrpaths.vrpath").read_text())
        self.assertEqual(registry["external_drivers"], [str(driver)])

    def test_prefers_extended_variant(self):
        self.driver()
        preferred = self.driver(extended=True)
        self.assertIn(str(preferred), self.success().stderr)

    def test_wrong_frequency_is_not_silently_selected(self):
        self.driver(hz=90)
        result = self.run_launcher()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("No matching 60 Hz", result.stderr)
        self.assertFalse(self.config.exists())

    def test_multiple_devices_need_a_profile(self):
        self.driver("alpha")
        self.driver("beta")
        self.assertIn("Multiple device profiles", self.run_launcher().stderr)
        self.success(XR_OPENVR_DEVICE="beta")

    def test_registered_profile_is_kept_when_several_devices_exist(self):
        self.driver("alpha")
        self.driver("beta")
        driver = self.driver("beta", hz=90)
        registry = self.home / ".config/openvr/openvrpaths.vrpath"
        registry.parent.mkdir(parents=True)
        registry.write_text(json.dumps({"external_drivers": [str(driver)]}))
        self.assertIn("device=beta", self.success().stderr)

    def test_preserves_window_position_and_unrelated_settings_and_backup(self):
        self.driver()
        self.config.parent.mkdir(parents=True)
        original = {"xr_tracking": {"windowX": -3840, "windowY": 120,
                                     "runtimePoseTransport": "tcp"}, "custom": {"keep": 1}}
        self.config.write_text(json.dumps(original))
        self.success()
        config = json.loads(self.config.read_text())
        self.assertEqual(config["xr_tracking"]["windowX"], -3840)
        self.assertEqual(config["steamvr"]["windowY"], 120)
        self.assertEqual(config["xr_tracking"]["runtimePoseTransport"], "tcp")
        self.assertEqual(config["custom"], {"keep": 1})
        self.assertEqual(json.loads(self.config.with_name(self.config.name + ".before-xr-gate").read_text()), original)

    def test_direct_mode_clears_linux_fallback(self):
        self.driver()
        self.success()
        self.success(XR_OPENVR_DISPLAY_MODE="direct")
        config = json.loads(self.config.read_text())
        self.assertFalse(config["xr_tracking"]["isDisplayOnDesktop"])
        self.assertFalse(config["steamvr"]["allowFallbackMirrorWindowLinux"])
        self.assertTrue(config["direct_mode"]["enable"])

    def test_wayland_does_not_call_xrandr_without_output(self):
        self.driver()
        self.assertIn("experimental", self.success(XDG_SESSION_TYPE="wayland", WAYLAND_DISPLAY="wayland-0").stderr)

    def test_wayland_output_override_is_rejected_before_settings_change(self):
        self.driver()
        result = self.run_launcher(XDG_SESSION_TYPE="wayland", XR_OPENVR_OUTPUT="DP-2")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("configure displays in your compositor", result.stderr)
        self.assertFalse(self.config.exists())

    def test_x11_optional_output_uses_actual_coordinates(self):
        self.driver()
        self.stub("xrandr", '''case "$1" in
--query) echo 'DP-2 connected 3840x1080-3840+120 (normal)' ;;
*) : ;;
esac
''')
        self.success(XR_OPENVR_OUTPUT="DP-2")
        config = json.loads(self.config.read_text())
        self.assertEqual(config["xr_tracking"]["windowX"], -3840)
        self.assertEqual(config["steamvr"]["windowY"], 120)

    def test_disconnected_output_is_rejected(self):
        self.driver()
        self.stub("xrandr", "echo 'DP-2 disconnected'\n")
        result = self.run_launcher(XR_OPENVR_OUTPUT="DP-2")
        self.assertIn("not connected", result.stderr)
        self.assertNotEqual(result.returncode, 0)

    def test_steam_inherits_gpu_selection_without_forcing_nvidia(self):
        self.driver()
        result = self.success(XR_OPENVR_LAUNCH_MODE="steam", VK_DRIVER_FILES="/existing.json")
        env = json.loads(next(line[4:] for line in result.stdout.splitlines() if line.startswith("ENV=")))
        self.assertEqual(env["VK_DRIVER_FILES"], "/existing.json")
        self.assertNotIn("VK_ICD_FILENAMES", env)
        self.assertNotIn("__GLX_VENDOR_LIBRARY_NAME", env)

    def test_custom_config_location_created_on_first_launch(self):
        self.driver()
        registry = self.home / ".config/openvr/openvrpaths.vrpath"
        registry.parent.mkdir(parents=True)
        custom = self.home / "custom-config"
        registry.write_text(json.dumps({"config": [str(custom)]}))
        self.success()
        self.assertTrue((custom / "steamvr.vrsettings").exists())

    def test_legacy_package_without_profile(self):
        self.driver(profile="", legacy=True)
        self.success()

    def test_relocated_release_entrypoint(self):
        self.driver()
        script = self.package / "bin/scripts/drivers/steam_vr/start_openvr.py"
        script.parent.mkdir(parents=True)
        shutil.copy2(LAUNCHER, script)
        script.chmod(0o755)
        wrapper = self.package / "run_openvr.sh"
        wrapper.write_text('#!/bin/sh\nexec "$(dirname "$0")/bin/scripts/drivers/steam_vr/start_openvr.py" "$@"\n')
        wrapper.chmod(0o755)
        env = dict(self.env)
        env.pop("XR_PACKAGE_ROOT")
        result = subprocess.run([str(wrapper)], env=env, text=True, capture_output=True, timeout=20)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertTrue(self.config.exists())

    def test_running_steam_can_block_changes(self):
        self.driver()
        self.stub("pgrep", "exit 0\n")
        result = self.run_launcher(XR_OPENVR_STOP_STEAM="0")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Close SteamVR and Steam", result.stderr)
        self.assertFalse(self.config.exists())

    def test_explicit_registrar_and_missing_override(self):
        self.driver()
        self.success(XR_OPENVR_REGISTER_SCRIPT=str(REGISTRAR))
        result = self.run_launcher(XR_OPENVR_REGISTER_SCRIPT=str(self.root / "missing.sh"))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("XR_OPENVR_REGISTER_SCRIPT not found", result.stderr)

    def test_invalid_frequency_fails_before_settings_change(self):
        self.driver()
        for value in ("nan", "inf", "0", "oops"):
            with self.subTest(value=value):
                self.assertNotEqual(self.run_launcher(XR_DISPLAY_FREQUENCY_HZ=value).returncode, 0)
        self.assertFalse(self.config.exists())

    def test_generator_writes_both_display_modes(self):
        source = ROOT / "drivers/openvr_driver/resources/settings/default.vrsettings"
        for mode in ("extended_sbs", "direct"):
            out = self.root / f"{mode}.vrsettings"
            result = subprocess.run([sys.executable, str(ROOT / "drivers/openvr_driver/scripts/render_display_settings.py"),
                                     "--settings", str(source), "--output", str(out),
                                     "--display-frequency", "60", "--display-mode", mode],
                                    env=self.env, capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            config = json.loads(out.read_text())
            self.assertEqual(config["steamvr"]["allowFallbackMirrorWindowLinux"], mode == "extended_sbs")
            self.assertEqual(config["direct_mode"]["enable"], mode == "direct")


if __name__ == "__main__":
    unittest.main()
