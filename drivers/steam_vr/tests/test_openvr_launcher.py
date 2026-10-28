import json
import hashlib
import os
from pathlib import Path
import pty
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

    def run_launcher(self, args=(), **env):
        return subprocess.run([sys.executable, str(LAUNCHER), *args], env=dict(self.env, **env),
                              stdin=subprocess.DEVNULL, text=True, capture_output=True, timeout=20)

    def success(self, **env):
        result = self.run_launcher(**env)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return result

    def randr_displays(self, headset="DP-2", edid=True, fail_args=None):
        headset_edid = bytes.fromhex("00ffffffffffff00" + "01" * 120)
        desktop_edid = bytes.fromhex("00ffffffffffff00" + "02" * 120)

        def edid_lines(data):
            value = data.hex()
            return "\tEDID:\n" + "\n".join("\t\t" + value[i:i + 32] for i in range(0, len(value), 32)) + "\n"

        query = ("DP-6 connected primary 1920x1080+0+0 (normal left inverted right x axis y axis) 1390mm x 390mm\n"
                 + edid_lines(desktop_edid) + "\tnon-desktop: 0\n   1920x1080 60.00*+\n"
                 + f"{headset} connected 3840x1080+1920+0 (normal left inverted right x axis y axis) 120mm x 70mm\n"
                 + (edid_lines(headset_edid) if edid else "")
                 + "\tnon-desktop: 0\n   3840x1080 60.00*+ 90.00\n"
                 + "DP-7 disconnected (normal left inverted right x axis y axis)\n")
        self.randr_commands = self.root / "randr-commands.jsonl"
        script = (f"#!{sys.executable}\nimport json, sys\nfrom pathlib import Path\n"
                  f"args = sys.argv[1:]\nwith Path({str(self.randr_commands)!r}).open('a') as log:\n"
                  "    log.write(json.dumps(args) + '\\n')\n"
                  f"if args == {fail_args!r}:\n    sys.exit(42)\n"
                  f"if args in (['--prop'], ['--query']):\n    print({query!r}, end='')\n")
        path = self.commands / "xrandr"
        path.write_text(script)
        path.chmod(0o755)
        return hashlib.sha256(headset_edid).hexdigest()

    def randr_calls(self):
        return [json.loads(line) for line in self.randr_commands.read_text().splitlines()]

    def direct_launcher(self, selection=None, args=(), **env):
        values = dict(self.env, XR_OPENVR_DISPLAY_MODE="direct", XR_OPENVR_LAUNCH_MODE="steamvr", DISPLAY=":42")
        values.update(env)
        command = [sys.executable, str(LAUNCHER), *args]
        if selection is None:
            return subprocess.run(command, env=values, stdin=subprocess.DEVNULL,
                                  text=True, capture_output=True, timeout=20)
        master, slave = pty.openpty()
        try:
            process = subprocess.Popen(command, env=values, stdin=slave,
                                       stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            try:
                os.write(master, selection.encode())
                stdout, stderr = process.communicate(timeout=20)
                return subprocess.CompletedProcess(command, process.returncode, stdout, stderr)
            finally:
                if process.poll() is None:
                    process.kill()
                    process.communicate()
        finally:
            os.close(master)
            os.close(slave)

    def direct_cache(self):
        return self.home / ".config/xr-gate/steamvr-direct-output.json"

    def test_direct_interactive_choice_releases_only_selected_display_and_is_saved(self):
        self.driver()
        edid = self.randr_displays()
        result = self.direct_launcher(selection="invalid\n2\n")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        calls = self.randr_calls()
        self.assertIn(["--output", "DP-2", "--set", "non-desktop", "1"], calls)
        self.assertIn(["--output", "DP-2", "--off"], calls)
        self.assertTrue(all(call[1] == "DP-2" for call in calls if call[0] == "--output"))
        self.assertIn("[primary]", result.stderr)
        self.assertEqual(json.loads(self.direct_cache().read_text())["devices"]["alpha"],
                         {"output": "DP-2", "edid": edid})
        config = json.loads(self.config.read_text())
        self.assertFalse(config["xr_tracking"]["isDisplayOnDesktop"])
        self.assertFalse(config["steamvr"]["allowFallbackMirrorWindowLinux"])

    def test_direct_reuses_extended_package_with_requested_rate_and_direct_settings(self):
        driver = self.driver(extended=True)
        package_settings = driver / "resources/settings/default.vrsettings"
        original = package_settings.read_bytes()
        self.randr_displays()
        result = self.direct_launcher(XR_OPENVR_OUTPUT="DP-2", XR_DISPLAY_FREQUENCY_HZ="72")
        self.assertEqual(result.returncode, 0, result.stderr)
        config = json.loads(self.config.read_text())
        self.assertFalse(config["xr_tracking"]["isDisplayOnDesktop"])
        self.assertTrue(config["steamvr"]["directMode"])
        self.assertFalse(config["steamvr"]["allowFallbackMirrorWindowLinux"])
        self.assertEqual(config["xr_tracking"]["displayFrequency"], 72)
        self.assertEqual(config["steamvr"]["preferredRefreshRate"], 72)
        self.assertEqual(package_settings.read_bytes(), original)
        self.assertIn(["--output", "DP-2", "--off"], self.randr_calls())

    def test_direct_unwritable_choice_cache_does_not_block_steam_launch(self):
        self.driver()
        self.randr_displays()
        blocked = self.root / "not-a-directory"
        blocked.write_text("keep")
        result = self.direct_launcher(XR_OPENVR_OUTPUT="DP-2", XDG_CONFIG_HOME=str(blocked))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("Could not save the display choice", result.stderr)
        self.assertIn("ENV=", result.stdout)

    def test_direct_saved_choice_follows_edid_when_connector_name_changes(self):
        self.driver()
        self.randr_displays()
        first = self.direct_launcher(XR_OPENVR_OUTPUT="DP-2")
        self.assertEqual(first.returncode, 0, first.stderr)
        self.randr_commands.unlink()
        self.randr_displays(headset="DP-4")
        result = self.direct_launcher()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("Using saved headset display: DP-4", result.stderr)
        self.assertIn(["--output", "DP-4", "--off"], self.randr_calls())
        self.assertNotIn(["--output", "DP-6", "--off"], self.randr_calls())

    def test_direct_stale_edid_does_not_disable_replacement_monitor(self):
        self.driver()
        self.randr_displays()
        self.direct_cache().parent.mkdir(parents=True)
        self.direct_cache().write_text(json.dumps({"devices": {"alpha": {"output": "DP-2", "edid": "different"}}}))
        result = self.direct_launcher()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("--choose-output", result.stderr)
        self.assertFalse(self.config.exists())
        self.assertFalse(any(call[0] == "--output" for call in self.randr_calls()))

    def test_direct_noninteractive_first_launch_needs_explicit_choice(self):
        self.driver()
        self.randr_displays()
        result = self.direct_launcher()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("XR_OPENVR_OUTPUT", result.stderr)
        self.assertFalse(self.config.exists())
        self.assertEqual(self.randr_calls(), [["--prop"]])

    def test_direct_can_choose_again_instead_of_reusing_saved_display(self):
        self.driver()
        self.randr_displays()
        self.assertEqual(self.direct_launcher(XR_OPENVR_OUTPUT="DP-2").returncode, 0)
        result = self.direct_launcher(selection="1\n", args=("--choose-output",))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads(self.direct_cache().read_text())["devices"]["alpha"]["output"], "DP-6")

    def test_direct_explicit_output_alias_is_supported(self):
        self.driver()
        self.randr_displays()
        result = self.direct_launcher(XR_OPENVR_DGPU_OUTPUT="DP-2")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(["--output", "DP-2", "--off"], self.randr_calls())

    def test_direct_output_without_edid_is_not_reused_automatically(self):
        self.driver()
        self.randr_displays(edid=False)
        result = self.direct_launcher(selection="DP-2\n")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("EDID is unavailable", result.stderr)
        self.assertNotIn("alpha", json.loads(self.direct_cache().read_text())["devices"])
        self.assertNotEqual(self.direct_launcher().returncode, 0)

    def test_direct_cancelled_choice_does_not_change_outputs_or_registration(self):
        self.driver()
        self.randr_displays()
        result = self.direct_launcher(selection="\n")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("cancelled", result.stderr)
        self.assertEqual(self.randr_calls(), [["--prop"]])
        self.assertFalse(self.config.exists())

    def test_direct_registration_only_and_manual_preparation_do_not_touch_displays(self):
        self.driver()
        for env in ({"XR_OPENVR_LAUNCH_MODE": "none", "XR_OPENVR_OUTPUT": "DP-2"},
                    {"XR_OPENVR_SET_DISPLAY": "0"}):
            with self.subTest(env=env):
                result = self.direct_launcher(**env)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertNotIn("unexpected xrandr", result.stderr)

    def test_direct_wayland_cannot_choose_output_or_modify_displays(self):
        self.driver()
        result = self.direct_launcher(args=("--choose-output",), XDG_SESSION_TYPE="wayland", WAYLAND_DISPLAY="wayland-0")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("On Wayland", result.stderr)
        self.assertFalse(self.config.exists())
        self.assertNotIn("unexpected xrandr", result.stderr)

    def test_direct_missing_display_fails_before_closing_steam(self):
        self.driver()
        self.stub("pgrep", "exit 0\n")
        result = self.direct_launcher(DISPLAY="")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("X11 DISPLAY", result.stderr)
        self.assertNotIn("Closing Steam", result.stderr)
        self.assertFalse(self.config.exists())

    def test_direct_registration_failure_restores_output_property_mode_rate_and_position(self):
        self.driver()
        self.randr_displays()
        registrar = self.root / "failing-register.sh"
        registrar.write_text("exit 23\n")
        result = self.direct_launcher(XR_OPENVR_OUTPUT="DP-2", XR_OPENVR_REGISTER_SCRIPT=str(registrar))
        self.assertNotEqual(result.returncode, 0)
        calls = self.randr_calls()
        self.assertIn(["--output", "DP-2", "--off"], calls)
        self.assertIn(["--output", "DP-2", "--set", "non-desktop", "0"], calls)
        self.assertEqual(calls[-1], ["--output", "DP-2", "--mode", "3840x1080", "--pos", "1920x0",
                                     "--rotate", "normal", "--rate", "60.00"])
        self.assertIn("Restoring desktop display", result.stderr)

    def test_direct_output_configuration_failure_restores_previous_state(self):
        self.driver()
        self.randr_displays(fail_args=["--output", "DP-2", "--off"])
        result = self.direct_launcher(XR_OPENVR_OUTPUT="DP-2")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(["--output", "DP-2", "--set", "non-desktop", "0"], self.randr_calls())
        self.assertIn("--mode", self.randr_calls()[-1])
        self.assertFalse(self.config.exists())

    def test_direct_steam_execution_failure_restores_output(self):
        self.driver()
        self.randr_displays()
        (self.commands / "steam").write_bytes(b"\x00")
        result = self.direct_launcher(XR_OPENVR_OUTPUT="DP-2")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Restoring desktop display", result.stderr)
        self.assertIn("--mode", self.randr_calls()[-1])

    def test_direct_help_and_positional_frequency_work_with_display_choice(self):
        help_result = self.run_launcher(args=("--help",))
        self.assertEqual(help_result.returncode, 0, help_result.stderr)
        self.assertIn("--choose-output", help_result.stdout)
        self.driver()
        self.randr_displays()
        result = self.direct_launcher(selection="2\n", args=("59.94", "--choose-output"), XR_DISPLAY_FREQUENCY_HZ="")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads(self.config.read_text())["xr_tracking"]["displayFrequency"], 59.94)

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

    def test_package_frequency_can_be_overridden_without_modifying_package(self):
        driver = self.driver(hz=60)
        settings = driver / "resources/settings/default.vrsettings"
        original = settings.read_bytes()
        for mode in ("extended_sbs", "direct"):
            for hz in (90, 72, 59.94):
                with self.subTest(mode=mode, hz=hz):
                    self.success(XR_OPENVR_DISPLAY_MODE=mode, XR_DISPLAY_FREQUENCY_HZ=str(hz))
                    config = json.loads(self.config.read_text())
                    self.assertEqual(config["xr_tracking"]["displayFrequency"], hz)
                    self.assertEqual(config["steamvr"]["displayFrequency"], hz)
                    self.assertEqual(config["steamvr"]["preferredRefreshRate"], hz)
                    self.assertEqual(config["xr_tracking"]["isDisplayOnDesktop"], mode == "extended_sbs")
                    registry = json.loads((self.home / ".config/openvr/openvrpaths.vrpath").read_text())
                    self.assertEqual(registry["external_drivers"], [str(driver)])
                    self.assertEqual(settings.read_bytes(), original)

    def test_matching_frequency_package_is_still_preferred(self):
        self.driver(hz=60, extended=True)
        preferred = self.driver(hz=90)
        self.success(XR_DISPLAY_FREQUENCY_HZ="90")
        registry = json.loads((self.home / ".config/openvr/openvrpaths.vrpath").read_text())
        self.assertEqual(registry["external_drivers"], [str(preferred)])

    def test_fallback_package_prefers_mode_then_lowest_default_frequency(self):
        self.driver(hz=60)
        self.driver(hz=120, extended=True)
        preferred = self.driver(hz=90, extended=True)
        self.success(XR_DISPLAY_FREQUENCY_HZ="72")
        registry = json.loads((self.home / ".config/openvr/openvrpaths.vrpath").read_text())
        self.assertEqual(registry["external_drivers"], [str(preferred)])
        self.assertEqual(json.loads(self.config.read_text())["xr_tracking"]["displayFrequency"], 72)

    def test_explicit_package_can_have_another_default_frequency(self):
        selected = self.driver(hz=60)
        self.driver(hz=90, extended=True)
        for override in ({"DRIVER_PACKAGE": str(selected)},
                         {"XR_OPENVR_DRIVER_DIR_NAME": selected.parent.name}):
            with self.subTest(override=override):
                self.success(XR_DISPLAY_FREQUENCY_HZ="90", **override)
                registry = json.loads((self.home / ".config/openvr/openvrpaths.vrpath").read_text())
                self.assertEqual(registry["external_drivers"], [str(selected)])
                self.assertEqual(json.loads(self.config.read_text())["xr_tracking"]["displayFrequency"], 90)

    def test_frequency_does_not_implicitly_select_another_device(self):
        self.driver("alpha", hz=60)
        self.driver("beta", hz=90)
        result = self.run_launcher()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Multiple device profiles", result.stderr)
        self.assertFalse(self.config.exists())
        self.success(XR_OPENVR_DEVICE="beta")

    def test_registered_device_is_kept_even_without_matching_frequency(self):
        self.driver("alpha", hz=60)
        registered = self.driver("beta", hz=90)
        registry = self.home / ".config/openvr/openvrpaths.vrpath"
        registry.parent.mkdir(parents=True)
        registry.write_text(json.dumps({"external_drivers": [str(registered)]}))
        self.success()
        self.assertEqual(json.loads(registry.read_text())["external_drivers"], [str(registered)])
        self.assertEqual(json.loads(self.config.read_text())["xr_tracking"]["displayFrequency"], 60)

    def test_ambiguous_packages_still_require_explicit_selection(self):
        driver = self.driver(hz=60)
        duplicate = driver.parent.with_name("custom_build") / "xr_tracking"
        shutil.copytree(driver, duplicate)
        result = self.run_launcher(XR_DISPLAY_FREQUENCY_HZ="72")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Multiple matching driver packages", result.stderr)
        self.assertFalse(self.config.exists())
        self.success(XR_DISPLAY_FREQUENCY_HZ="72", DRIVER_PACKAGE=str(driver))

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

    def test_wayland_windowed_launch_uses_xwayland(self):
        self.driver()
        result = self.success(XR_OPENVR_LAUNCH_MODE="steamvr", XDG_SESSION_TYPE="wayland",
                              WAYLAND_DISPLAY="wayland-0", DISPLAY=":42", SDL_VIDEODRIVER="wayland",
                              XDG_CURRENT_DESKTOP="GNOME", VK_DRIVER_FILES="/existing.json")
        env = json.loads(next(line[4:] for line in result.stdout.splitlines() if line.startswith("ENV=")))
        self.assertEqual(env["SDL_VIDEODRIVER"], "x11")
        self.assertNotIn("WAYLAND_DISPLAY", env)
        self.assertEqual(env["XDG_SESSION_TYPE"], "x11")
        self.assertEqual(env["DISPLAY"], ":42")
        self.assertEqual(env["XDG_CURRENT_DESKTOP"], "GNOME")
        self.assertEqual(env["VK_DRIVER_FILES"], "/existing.json")
        self.assertNotIn("VK_ICD_FILENAMES", env)
        self.assertNotIn("SDL_VIDEO_MINIMIZE_ON_FOCUS_LOSS", env)

    def test_wayland_registration_only_explains_manual_xwayland_start(self):
        self.driver()
        result = self.success(XDG_SESSION_TYPE="wayland", WAYLAND_DISPLAY="wayland-0", DISPLAY="")
        self.assertTrue(self.config.exists())
        self.assertNotIn("ENV=", result.stdout)
        self.assertIn("SDL_VIDEODRIVER=x11 steam", result.stderr)

    def test_wayland_launch_requires_display_before_closing_steam_or_writing_settings(self):
        self.driver()
        self.stub("pgrep", "exit 0\n")
        result = self.run_launcher(XR_OPENVR_LAUNCH_MODE="steamvr", XDG_SESSION_TYPE="wayland",
                                   WAYLAND_DISPLAY="wayland-0", DISPLAY="")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("DISPLAY", result.stderr)
        self.assertNotIn("Closing Steam", result.stderr)
        self.assertNotIn("ENV=", result.stdout)
        self.assertFalse(self.config.exists())

    def test_direct_wayland_launch_keeps_native_backend(self):
        self.driver()
        result = self.success(XR_OPENVR_DISPLAY_MODE="direct", XR_OPENVR_LAUNCH_MODE="steam",
                              XDG_SESSION_TYPE="wayland", WAYLAND_DISPLAY="wayland-0", DISPLAY="",
                              SDL_VIDEODRIVER="wayland")
        env = json.loads(next(line[4:] for line in result.stdout.splitlines() if line.startswith("ENV=")))
        self.assertEqual(env["XDG_SESSION_TYPE"], "wayland")
        self.assertEqual(env["WAYLAND_DISPLAY"], "wayland-0")
        self.assertEqual(env["SDL_VIDEODRIVER"], "wayland")

    def test_native_x11_launch_keeps_session_environment(self):
        self.driver()
        result = self.success(XR_OPENVR_LAUNCH_MODE="steam", DISPLAY=":42", SDL_VIDEODRIVER="x11")
        env = json.loads(next(line[4:] for line in result.stdout.splitlines() if line.startswith("ENV=")))
        self.assertEqual(env["XDG_SESSION_TYPE"], "x11")
        self.assertEqual(env["SDL_VIDEODRIVER"], "x11")
        self.assertEqual(env["DISPLAY"], ":42")

    def test_registration_replaces_stale_preferred_rate_but_keeps_diagnostic_settings(self):
        self.driver(hz=60)
        self.driver(hz=90)
        self.config.parent.mkdir(parents=True)
        self.config.write_text(json.dumps({"steamvr": {"preferredRefreshRate": 144,
                                                     "disableLinuxWaitForPresent": True,
                                                     "enableLinuxVulkanAsync": False}}))
        for hz in (60, 90):
            with self.subTest(hz=hz):
                self.success(XR_DISPLAY_FREQUENCY_HZ=str(hz))
                steamvr = json.loads(self.config.read_text())["steamvr"]
                self.assertEqual(steamvr["preferredRefreshRate"], hz)
                self.assertEqual(steamvr["displayFrequency"], hz)
                self.assertTrue(steamvr["disableLinuxWaitForPresent"])
                self.assertFalse(steamvr["enableLinuxVulkanAsync"])

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
        stale = json.loads(source.read_text())
        stale["steamvr"]["preferredRefreshRate"] = 144
        source = self.root / "stale.vrsettings"
        source.write_text(json.dumps(stale))
        for mode in ("extended_sbs", "direct"):
            for hz in (60, 90):
                with self.subTest(mode=mode, hz=hz):
                    out = self.root / f"{mode}-{hz}.vrsettings"
                    result = subprocess.run([sys.executable, str(ROOT / "drivers/openvr_driver/scripts/render_display_settings.py"),
                                             "--settings", str(source), "--output", str(out),
                                             "--display-frequency", str(hz), "--display-mode", mode],
                                            env=self.env, capture_output=True, text=True, timeout=10)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    config = json.loads(out.read_text())
                    self.assertEqual(config["steamvr"]["allowFallbackMirrorWindowLinux"], mode == "extended_sbs")
                    self.assertEqual(config["direct_mode"]["enable"], mode == "direct")
                    self.assertEqual(config["xr_tracking"]["displayFrequency"], hz)
                    self.assertEqual(config["steamvr"]["displayFrequency"], hz)
                    self.assertEqual(config["steamvr"]["preferredRefreshRate"], hz)


if __name__ == "__main__":
    unittest.main()
