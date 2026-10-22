#!/usr/bin/env python3
"""Verify source-age configuration in the real binary used by old launchers."""
import argparse
import os
from pathlib import Path
import re
import socket
import subprocess
import unittest

ADAPTER = None


def run(settings=None, options=()):
    env = {k: v for k, v in os.environ.items() if k != 'MAX_HMD_AGE_MS'}
    env.update(settings or {})
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.bind(('127.0.0.1', 0))
        port = sock.getsockname()[1]
    return subprocess.run([ADAPTER, '--input', 'udp', '--udp-bind-host', '127.0.0.1',
        '--udp-bind-port', str(port), '--mode', 'tick', '--duration', '0.03',
        '--print-every', '0', *options], env=env, capture_output=True, text=True, timeout=3)


class HmdAgeTest(unittest.TestCase):
    def age(self, result):
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        match = re.search(r'^max_hmd_age_ms:\s*(\S+)', result.stdout, re.MULTILINE)
        self.assertIsNotNone(match, result.stdout)
        return float(match.group(1))

    def test_default_and_environment_work_without_launcher_changes(self):
        self.assertEqual(self.age(run()), 250)
        self.assertEqual(self.age(run({'MAX_HMD_AGE_MS': '500'})), 500)

    def test_explicit_command_line_takes_precedence(self):
        self.assertEqual(self.age(run({'MAX_HMD_AGE_MS': '500'}, ['--max-hmd-age-ms', '300'])), 300)

    def test_invalid_environment_is_rejected(self):
        self.assertNotEqual(run({'MAX_HMD_AGE_MS': 'invalid'}).returncode, 0)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--adapter', required=True)
    args, remaining = parser.parse_known_args()
    ADAPTER = str(Path(args.adapter).resolve())
    unittest.main(argv=[__file__, *remaining])
