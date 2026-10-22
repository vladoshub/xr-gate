import importlib.util
from pathlib import Path
import subprocess
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

SPEC = importlib.util.spec_from_file_location('xr_gate_readiness', Path(__file__).resolve().parents[1] / 'linux/xr_gate_wsl.py')
linux = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(linux)


class ReadinessTest(unittest.TestCase):
    def runtime(self, folder):
        account = SimpleNamespace(pw_uid=1000, pw_gid=1000, pw_name='fixture_user', pw_dir=str(folder))
        with patch.object(linux.pwd, 'getpwnam', return_value=account):
            return linux.Runtime({'user':'fixture_user', 'settings':{'XR_LINUX_ROOT':str(folder), 'XR_BLUETOOTH_READY_TIMEOUT':'1'}})

    def test_canonical_root_is_rejected_before_any_write(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'root-link').symlink_to('/')
            for path in ('/', '/tmp/../..', str(root / 'root-link')):
                with self.subTest(path=path), patch.object(linux.pwd, 'getpwnam', return_value=SimpleNamespace(pw_uid=1000, pw_name='fixture_user', pw_dir=directory)), patch.object(linux.os, 'chown') as chown:
                    with self.assertRaisesRegex(ValueError, 'filesystem root'):
                        linux.Runtime({'user':'fixture_user','settings':{'XR_LINUX_ROOT':path}})
                    chown.assert_not_called()
            runtime = self.runtime(root / 'managed')
            runtime.base.symlink_to('/')
            with patch.object(linux.os, 'geteuid', return_value=0), patch.object(linux.os, 'chown') as chown:
                with self.assertRaisesRegex(ValueError, 'filesystem root'):
                    runtime.root_required()
                chown.assert_not_called()

    def test_bluetooth_wait_requires_a_controller_and_retries_until_ready(self):
        with tempfile.TemporaryDirectory() as directory:
            runtime = self.runtime(Path(directory))
            clock = [0.0]
            def advance(seconds): clock[0] += seconds
            empty = subprocess.CompletedProcess([], 0, stdout='', stderr='')
            ready = subprocess.CompletedProcess([], 0, stdout='Controller AA:BB:CC:DD:EE:FF Test dongle [default]\n', stderr='')
            with patch.object(linux.time, 'monotonic', side_effect=lambda:clock[0]), patch.object(linux.time, 'sleep', side_effect=advance), patch.object(linux.subprocess, 'run', return_value=empty):
                with self.assertRaisesRegex(RuntimeError, 'no controller became ready'):
                    runtime.wait_for_bluetooth()
            clock[0] = 0
            with patch.object(linux.time, 'monotonic', side_effect=lambda:clock[0]), patch.object(linux.time, 'sleep', side_effect=advance), patch.object(linux.subprocess, 'run', side_effect=[empty,ready]) as query:
                runtime.wait_for_bluetooth()
                self.assertEqual(query.call_count, 2)

    def test_disabled_bluetooth_does_not_require_hci(self):
        with tempfile.TemporaryDirectory() as directory:
            runtime = self.runtime(Path(directory))
            runtime.cfg['bluetooth'] = False
            with patch.object(runtime, 'root_required'), patch.object(runtime, 'device_nodes', return_value=([],[],[])), patch.object(runtime, 'wait_for_bluetooth') as wait, patch.object(linux, 'run'):
                runtime.prepare()
                wait.assert_not_called()

    def test_selected_usb_dongle_must_be_ready_even_with_another_adapter_present(self):
        with tempfile.TemporaryDirectory() as directory:
            runtime = self.runtime(Path(directory))
            runtime.cfg['bluetooth_usb_id'] = '1234:abcd'
            clock = [0.0]
            def advance(seconds): clock[0] += seconds
            with patch.object(linux.time,'monotonic',side_effect=lambda:clock[0]), patch.object(linux.time,'sleep',side_effect=advance), patch.object(runtime,'firmware',side_effect=[{'ready':False},{'ready':True,'address':'AA:BB:CC:DD:EE:FF'}]) as probe, patch.object(linux.subprocess,'run') as generic:
                runtime.wait_for_bluetooth()
                self.assertEqual(probe.call_count,2)
                generic.assert_not_called()


if __name__ == '__main__': unittest.main()
