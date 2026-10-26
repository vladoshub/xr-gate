import importlib.util
import errno
import json
import os
from pathlib import Path
import pwd
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import Mock, patch

HELPER = Path(__file__).resolve().parents[1] / 'linux/xr_gate_wsl.py'
spec = importlib.util.spec_from_file_location('wsl_device_access', HELPER)
linux = importlib.util.module_from_spec(spec)
spec.loader.exec_module(linux)


class DeviceAccessTest(unittest.TestCase):
    def runtime(self):
        runtime = linux.Runtime.__new__(linux.Runtime)
        runtime.cfg = {'user': 'fixture_user', 'settings': {'XR_REQUIRE_CAMERA': '1'}}
        runtime.settings = runtime.cfg['settings']
        runtime.user = 'fixture_user'
        runtime.root_required = lambda: None
        runtime.user_required = lambda: None
        self.camera = Mock(spec=Path)
        self.camera.__str__ = Mock(return_value='/dev/video4')
        self.camera.is_char_device.return_value = True
        runtime.device_nodes = lambda: ([self.camera], [self.camera], [])
        runtime.capture_device = Mock(return_value='/dev/video4')
        return runtime

    def test_preparation_finishes_queued_rules_before_verifying_user_access(self):
        runtime = self.runtime()
        events, allowed = [], [False]
        pending = [True]

        def native(args, **kwargs):
            if args[:2] == ['udevadm', 'settle']:
                events.append('finish-rules')
                pending[0] = False
                allowed[0] = False  # uaccess clears a non-session user's ACL.
            if args[0] == 'setfacl':
                self.assertEqual(args[-1], self.camera)
                events.append('grant-access')
                allowed[0] = True

        def user_probe(args, **kwargs):
            events.append('verify-user')
            if pending[0]:
                allowed[0] = False
            try:
                with patch.object(linux.os, 'access', return_value=allowed[0]):
                    result = runtime.device_access()
                return subprocess.CompletedProcess(args, 0, json.dumps(result), '')
            except PermissionError as error:
                return subprocess.CompletedProcess(args, 1, '', str(error))

        runtime.as_user = Mock(side_effect=user_probe)
        with patch.object(linux, 'run', side_effect=native), patch.object(linux, 'log') as log:
            runtime.prepare()
        self.assertEqual(events, ['finish-rules', 'grant-access', 'verify-user'])
        self.assertTrue(any('verified for fixture_user' in call.args[0] for call in log.call_args_list))
        runtime.capture_device.assert_called_once()

    def test_device_access_requires_read_write_and_does_not_probe_as_root(self):
        runtime = self.runtime()
        with patch.object(linux.os, 'access', return_value=False) as access:
            with self.assertRaisesRegex(PermissionError, 'fixture_user.*video4'):
                runtime.device_access()
        access.assert_called_once_with(self.camera, os.R_OK | os.W_OK)
        runtime.capture_device.assert_not_called()

    def test_preparation_retries_transient_access_loss_with_new_permissions(self):
        runtime = self.runtime()
        runtime.as_user = Mock(side_effect=[
            subprocess.CompletedProcess([], 1, '', 'Permission denied: /dev/video4'),
            subprocess.CompletedProcess([], 0, '{"camera":"/dev/video4"}', '')])
        with patch.object(linux, 'run') as native, patch.object(linux.time, 'sleep') as wait:
            runtime.prepare()
        self.assertEqual(runtime.as_user.call_count, 2)
        self.assertEqual(sum(call.args[0][0] == 'setfacl' for call in native.call_args_list), 2)
        wait.assert_called_once_with(.25)
        runtime.capture_device.assert_not_called()  # Parent root is not the probe.

    def test_persistent_permission_failure_is_bounded_and_never_reports_success(self):
        runtime = self.runtime()
        runtime.as_user = Mock(return_value=subprocess.CompletedProcess([], 1, '', 'Permission denied: /dev/video4'))
        clock = [0.0]
        def advance(seconds):
            clock[0] += seconds
        with patch.object(linux, 'run'), patch.object(linux.time, 'monotonic', side_effect=lambda: clock[0]), patch.object(linux.time, 'sleep', side_effect=advance), patch.object(linux, 'log') as log:
            with self.assertRaisesRegex(RuntimeError, 'fixture_user.*Permission denied.*video4'):
                runtime.prepare()
        self.assertLessEqual(clock[0], 8)
        self.assertFalse(any('verified' in call.args[0] or 'granted' in call.args[0] for call in log.call_args_list))

    def test_reenumeration_updates_the_granted_device_set_and_selected_camera(self):
        runtime = self.runtime()
        replacement = Mock(spec=Path)
        replacement.__str__ = Mock(return_value='/dev/video8')
        replacement.is_char_device.return_value = True
        runtime.device_nodes = Mock(side_effect=[([self.camera], [self.camera], []),
                                                 ([replacement], [replacement], [])])
        runtime.as_user = Mock(side_effect=[
            subprocess.CompletedProcess([], 1, '', 'Permission denied: /dev/video8'),
            subprocess.CompletedProcess([], 0, '{"camera":"/dev/video8"}', '')])
        with patch.object(linux, 'run') as native, patch.object(linux.time, 'sleep'), patch.object(linux, 'log') as log:
            runtime.prepare()
        grants = [call.args[0][-1] for call in native.call_args_list if call.args[0][0] == 'setfacl']
        self.assertEqual(grants, [self.camera, replacement])
        self.assertTrue(any('Capture camera: /dev/video8' in call.args[0] for call in log.call_args_list))

    def test_udev_timeout_does_not_grant_permissions_on_unfinished_nodes(self):
        for failure in (subprocess.CalledProcessError(1, ['udevadm']), subprocess.TimeoutExpired(['udevadm'], 10)):
            with self.subTest(failure=type(failure).__name__):
                runtime = self.runtime()
                runtime.as_user = Mock()
                commands = []
                def native(args, **kwargs):
                    commands.append(args)
                    if args[:2] == ['udevadm', 'settle']:
                        raise failure
                with patch.object(linux, 'run', side_effect=native), self.assertRaisesRegex(RuntimeError, 'setup did not finish'):
                    runtime.prepare()
                self.assertFalse(any(command[0] == 'setfacl' for command in commands))
                runtime.as_user.assert_not_called()

    def test_device_disappearing_between_grant_and_probe_is_not_ready(self):
        runtime = self.runtime()
        runtime.device_nodes = lambda: ([], [], [])
        with self.assertRaisesRegex(FileNotFoundError, 'camera disappeared'):
            runtime.device_access()
        runtime.capture_device.assert_not_called()
        runtime.settings = {'XR_REQUIRE_HIDRAW': '1'}
        with self.assertRaisesRegex(FileNotFoundError, 'HID disappeared'):
            runtime.device_access()

    def test_permission_check_uses_real_regular_user_credentials(self):
        if os.geteuid() == 0:
            if not shutil.which('runuser'):
                self.skipTest('runuser is required for a real privilege drop')
            account = pwd.getpwnam('nobody')
        else:
            account = pwd.getpwuid(os.geteuid())
        with tempfile.TemporaryDirectory() as folder:
            base = Path(folder)
            base.chmod(0o755)
            node = base / 'selected-device'
            node.write_text('permission fixture')
            if os.geteuid() == 0:
                try:
                    os.chown(node, account.pw_uid, account.pw_gid)
                except OSError as error:
                    if error.errno in (errno.EINVAL, errno.EPERM):
                        self.skipTest('The container does not map a non-root UID for a real privilege drop')
                    raise
            node.chmod(0o400)
            runtime = linux.Runtime({'user': account.pw_name, 'settings': {'XR_LINUX_ROOT': str(base)}})
            child_helper = base / 'xr_gate_wsl.py'
            child_helper.write_bytes(HELPER.read_bytes())
            # Replace only the character-device classification: real file
            # permissions and the production runuser path remain in effect.
            script = '''
import importlib.util, json, sys
from pathlib import Path
from unittest.mock import patch
spec = importlib.util.spec_from_file_location('access_child', sys.argv[1])
helper = importlib.util.module_from_spec(spec)
spec.loader.exec_module(helper)
runtime = helper.Runtime(json.loads(sys.argv[3]))
node = Path(sys.argv[2])
runtime.device_nodes = lambda: ([node], [], [])
with patch.object(Path, 'is_char_device', return_value=True):
    print(json.dumps(runtime.device_access()))
'''
            args = [sys.executable, '-c', script, str(child_helper), str(node), json.dumps(runtime.cfg)]
            if os.geteuid() == 0:
                with node.open('r+b'):
                    pass  # Root can open a node that the runtime cannot use.
            denied = runtime.as_user(args, check=False, capture_output=True, text=True, timeout=10)
            self.assertNotEqual(denied.returncode, 0, denied.stdout)
            self.assertIn('Read/write device access is unavailable to ' + account.pw_name, denied.stderr)
            node.chmod(0o600)
            granted = runtime.as_user(args, check=False, capture_output=True, text=True, timeout=10)
            self.assertEqual(granted.returncode, 0, granted.stderr)
            self.assertEqual(json.loads(granted.stdout), {'camera': ''})


if __name__ == '__main__':
    unittest.main()
