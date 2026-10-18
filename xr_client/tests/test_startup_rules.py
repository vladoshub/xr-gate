"""Startup policy selection, configuration and hardware-independent readiness."""
import json
import sys
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from xr_client.config_reader import load_config, parse_prestart_option
from xr_client.startup_rules import load_startup_rules
from xr_client.xr_backend_client import Launcher


class StartupRulesTest(unittest.TestCase):
    def test_bundled_rules_preserve_default_and_unknown_services_stay_generic(self):
        rules = load_startup_rules()
        rule = rules.get("xreal_display_helper")
        self.assertTrue(rule.enabled)
        self.assertTrue(rule.line_buffered_output)
        self.assertIsNone(rule.confirmation_regex.search("setting display mode: 9"))
        self.assertEqual(rule.confirmation_regex.search("alive; mode=9").group(1), "9")
        self.assertFalse(rules.get("custom_display").enabled)
        with self.assertRaisesRegex(ValueError, "unknown service startup rule"):
            rules.get("custom_display", "typo")

    def test_disabled_rules_do_not_need_a_file_or_yaml_dependency(self):
        with patch.dict(sys.modules, {"yaml": None}):
            rules = load_startup_rules(Path("/nonexistent/policy.yaml"), enabled=False)
        self.assertFalse(rules.get("anything", "anything").enabled)

    def test_yaml_disable_and_validation(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "rules.yaml"
            path.write_text("enabled: false\nservices: ignored\n")
            self.assertFalse(load_startup_rules(path).enabled)
            path.write_text("services:\n  display:\n    enabled: false\n")
            self.assertFalse(load_startup_rules(path).get("display").enabled)
            for text in ("[]", "enabled: 'false'", "services: []",
                         "services:\n  display:\n    confirmation_regex: '['\n",
                         "services:\n  display:\n    confirmation_regex: ready\n    match_expected_value: true\n",
                         "services:\n  display:\n    line_buffered_output: 'false'\n"):
                path.write_text(text)
                with self.subTest(text=text), self.assertRaises(ValueError):
                    load_startup_rules(path)

    def test_json_resolves_yaml_relative_to_json_and_selects_rule_by_name(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "client.json"
            yaml_path = Path(folder) / "rules.yaml"
            yaml_path.write_text("services:\n  vendor_display:\n    confirmation_regex: 'confirmed=(\\w+)'\n    match_expected_value: true\n")
            data = {"service_startup_rules": {"config": "rules.yaml"},
                    "prestart_control": {"options": [{"id": "start", "service_name": "renamed_display", "startup_rule": "vendor_display"}]}}
            path.write_text(json.dumps(data))
            with patch("xr_client.config_reader.default_config_dict", return_value={}), \
                 patch("xr_client.config_reader._load_config_device_env", return_value=folder):
                cfg = load_config(str(path))
                self.assertEqual(cfg.prestart_control.options[0].startup_rule, "vendor_display")
                self.assertTrue(cfg.service_startup_rules.get("renamed_display", "vendor_display").enabled)
                yaml_path.unlink()
                with self.assertRaises(FileNotFoundError):
                    load_config(str(path))
                data["service_startup_rules"]["enabled"] = False
                path.write_text(json.dumps(data))
                self.assertFalse(load_config(str(path)).service_startup_rules.enabled)

    def test_custom_readback_policy_rejects_wrong_value_and_alive_fallback(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            policy = root / "rules.yaml"
            policy.write_text("services:\n  vendor_display:\n    confirmation_regex: 'confirmed=(\\w+)'\n    match_expected_value: true\n")
            log_path = root / "display.log"
            log_path.write_text("setting target=stereo\nconfirmed=mono\n")
            launcher = Launcher(SimpleNamespace(log_dir=folder, service_startup_rules=load_startup_rules(policy)))
            clock = SimpleNamespace(now=0.0)

            def advance(_):
                clock.now += 0.1
                if clock.now >= 0.4:
                    log_path.write_text("confirmed=stereo\n")

            with patch.object(launcher, "is_service_running", return_value=True), \
                 patch("xr_client.xr_backend_client.time.monotonic", side_effect=lambda: clock.now), \
                 patch("xr_client.xr_backend_client.time.sleep", side_effect=advance):
                launcher.wait_prestart_readiness("renamed_display", log_path,
                                                ["confirmed=stereo", "target=stereo"], 1, 0.1, 0,
                                                startup_rule="vendor_display")
            self.assertGreaterEqual(clock.now, 0.4)

    def test_disabling_rules_restores_generic_alive_fallback_and_skips_stdbuf(self):
        from xr_client.config_reader import PrestartControlSpec, PrestartOptionSpec
        with tempfile.TemporaryDirectory() as folder:
            option = PrestartOptionSpec("start", "1", "Start", service_name="xreal_display_helper", command=["helper"])
            cfg = SimpleNamespace(log_dir=folder, root_project=folder,
                                  service_startup_rules=load_startup_rules(enabled=False),
                                  prestart_control=PrestartControlSpec(enabled=True, prompt=False, selected_option="start", options=[option]))
            launcher = Launcher(cfg, dry_run=True)
            with patch.object(launcher, "start_service", return_value=True) as start, \
                 patch("xr_client.xr_backend_client.shutil.which") as which:
                launcher.run_prestart_control()
                self.assertEqual(start.call_args.args[0].command, ["helper"])
                which.assert_not_called()
            clock = SimpleNamespace(now=0.0)
            with patch.object(launcher, "is_service_running", return_value=True), \
                 patch("xr_client.xr_backend_client.time.monotonic", side_effect=lambda: clock.now), \
                 patch("xr_client.xr_backend_client.time.sleep", side_effect=lambda _: setattr(clock, "now", clock.now + 0.1)):
                launcher.wait_prestart_readiness("xreal_display_helper", Path(folder) / "absent.log", ["confirmed"], 1, 0.2, 0)
            self.assertLess(clock.now, 1)

    def test_json_rule_selector_is_a_service_name(self):
        for invalid in (False, "", [], 42):
            with self.subTest(invalid=invalid), self.assertRaises(ValueError):
                parse_prestart_option({"startup_rule": invalid}, "/tmp", 0)

    @unittest.skipUnless(sys.platform.startswith("linux"), "Linux line buffering")
    def test_custom_rule_controls_line_buffering_and_can_be_disabled_per_service(self):
        from xr_client.config_reader import PrestartControlSpec, PrestartOptionSpec
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "rules.yaml"
            option = PrestartOptionSpec("start", "1", "Start", service_name="my_display_process",
                                       startup_rule="vendor_display", command=["helper"])
            for enabled in (True, False):
                with self.subTest(enabled=enabled):
                    path.write_text(f"services:\n  vendor_display:\n    enabled: {str(enabled).lower()}\n    line_buffered_output: true\n")
                    cfg = SimpleNamespace(log_dir=folder, root_project=folder,
                                          service_startup_rules=load_startup_rules(path),
                                          prestart_control=PrestartControlSpec(enabled=True, prompt=False, selected_option="start", options=[option]))
                    launcher = Launcher(cfg, dry_run=True)
                    with patch.object(launcher, "start_service", return_value=True) as start, \
                         patch("xr_client.xr_backend_client.shutil.which", return_value="/usr/bin/stdbuf") as which:
                        launcher.run_prestart_control()
                        expected = ["/usr/bin/stdbuf", "-oL", "-eL", "helper"] if enabled else ["helper"]
                        self.assertEqual(start.call_args.args[0].command, expected)
                        self.assertEqual(which.called, enabled)


if __name__ == "__main__":
    unittest.main()
