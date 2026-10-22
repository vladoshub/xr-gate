#!/usr/bin/env python3
"""Verify effective yaw configuration through the real executable, without XR hardware."""

import argparse
import os
from pathlib import Path
import re
import socket
import subprocess
import unittest


ADAPTER = None
PREFIX = "RUNTIME_CONTROLLER_IMU_YAW_"


def run_adapter(options=(), settings=None):
    environment = {k: v for k, v in os.environ.items() if not k.startswith(PREFIX)}
    environment.update(settings or {})
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.bind(("127.0.0.1", 0))
        port = sock.getsockname()[1]
    return subprocess.run(
        [ADAPTER, "--input", "udp", "--udp-bind-host", "127.0.0.1",
         "--udp-bind-port", str(port), "--mode", "tick", "--duration", "0.03",
         "--print-every", "0", *options],
        env=environment, capture_output=True, text=True, timeout=3)


class YawConfigurationTest(unittest.TestCase):
    def settings(self, result):
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        lines = [line for line in result.stdout.splitlines()
                 if "[xr_runtime_adapter][yaw-config]" in line]
        self.assertEqual(len(lines), 1, result.stdout)
        return dict(re.findall(r"(\w+)=([^\s]+)", lines[0]))

    def test_defaults_are_reported_without_periodic_status(self):
        config = self.settings(run_adapter())
        self.assertEqual(config["algorithm"], "relative_rotation_v2")
        self.assertEqual(config["mode"], "smooth")
        self.assertEqual(float(config["max_source_age_ms"]), 250)
        self.assertEqual(float(config["reference_stable_ms"]), 1000)
        self.assertEqual(float(config["reference_max_range_deg"]), 3)

    def test_environment_and_cli_override_work_with_old_launch_scripts(self):
        environment = {PREFIX + "MAX_SOURCE_AGE_MS": "320",
                       PREFIX + "REFERENCE_STABLE_MS": "450",
                       PREFIX + "REFERENCE_MAX_RANGE_DEG": "4.5"}
        config = self.settings(run_adapter(settings=environment))
        self.assertEqual(float(config["max_source_age_ms"]), 320)
        self.assertEqual(float(config["reference_stable_ms"]), 450)
        self.assertEqual(float(config["reference_max_range_deg"]), 4.5)
        config = self.settings(run_adapter([
            "--runtime-controller-imu-yaw-max-source-age-ms", "175",
            "--runtime-controller-imu-yaw-reference-stable-ms", "0",
            "--runtime-controller-imu-yaw-reference-max-range-deg", "2"], environment))
        self.assertEqual(float(config["max_source_age_ms"]), 175)
        self.assertEqual(float(config["reference_stable_ms"]), 0)
        self.assertEqual(float(config["reference_max_range_deg"]), 2)

    def test_invalid_age_and_startup_settings_are_rejected(self):
        for name, values in (("max-source-age-ms", ("0", "1001", "nan")),
                             ("reference-stable-ms", ("-1", "5001", "nan")),
                             ("reference-max-range-deg", ("0", "46", "nan"))):
            for value in values:
                with self.subTest(name=name, value=value):
                    result = run_adapter(["--runtime-controller-imu-yaw-" + name, value])
                    self.assertNotEqual(result.returncode, 0)
        self.assertNotEqual(run_adapter(settings={PREFIX + "REFERENCE_STABLE_MS": "nan"}).returncode, 0)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--adapter", required=True)
    args, remaining = parser.parse_known_args()
    ADAPTER = str(Path(args.adapter).resolve())
    unittest.main(argv=[__file__, *remaining])
