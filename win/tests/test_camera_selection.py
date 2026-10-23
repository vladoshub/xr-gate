import importlib.util
import json
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import Mock, patch

WIN = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('wsl_camera_runtime', WIN / 'linux/xr_gate_wsl.py')
linux = importlib.util.module_from_spec(spec)
spec.loader.exec_module(linux)


class CameraSelectionTest(unittest.TestCase):
    def runtime(self, settings):
        runtime = linux.Runtime.__new__(linux.Runtime)
        runtime.settings = settings
        runtime.device_nodes = lambda: ([], [Path('/dev/video4'), Path('/dev/video5')], [])
        return runtime

    def test_device_caps_exclude_metadata_without_streaming(self):
        def capability(_, __, buffer, ___):
            struct.pack_into('=II', buffer, 84, 0x80000001, 0x00800000)
        with patch.object(linux.os, 'open', return_value=10), patch.object(linux.os, 'close') as close, patch.object(linux.fcntl, 'ioctl', side_effect=capability):
            self.assertFalse(linux.is_video_capture(Path('/dev/video5')))
        close.assert_called_once_with(10)
        def capture(_, __, buffer, ___):
            struct.pack_into('=II', buffer, 84, 0x80000000, 1)
        with patch.object(linux.os, 'open', return_value=10), patch.object(linux.os, 'close'), patch.object(linux.fcntl, 'ioctl', side_effect=capture):
            self.assertTrue(linux.is_video_capture(Path('/dev/video4')))

    def test_replug_chooses_actual_capture_node_instead_of_video_zero_or_metadata(self):
        runtime = self.runtime({'XR_CAMERA_AUTO_SELECT': '1'})
        with patch.object(linux, 'is_video_capture', side_effect=lambda path: path.name == 'video4'):
            self.assertEqual(runtime.capture_device(), '/dev/video4')
        runtime.device_nodes = lambda: ([], [Path('/dev/video8'), Path('/dev/video9')], [])
        with patch.object(linux, 'is_video_capture', side_effect=lambda path: path.name == 'video8'):
            self.assertEqual(runtime.capture_device(), '/dev/video8')

    def test_explicit_camera_and_custom_dual_profile_are_preserved(self):
        runtime = self.runtime({'XR_CAMERA_DEVICE': '/dev/v4l/by-id/selected-video-index0'})
        with patch.object(linux, 'is_video_capture', return_value=True) as probe:
            self.assertEqual(runtime.capture_device(), '/dev/v4l/by-id/selected-video-index0')
            probe.assert_called_once_with(Path('/dev/v4l/by-id/selected-video-index0'))
        runtime.settings = {'XR_CAMERA_AUTO_SELECT': '0'}
        with patch.object(linux, 'is_video_capture') as probe:
            self.assertEqual(runtime.capture_device(), '')
            probe.assert_not_called()

    def test_ambiguous_missing_and_inaccessible_camera_fail_explicitly(self):
        runtime = self.runtime({'XR_CAMERA_AUTO_SELECT': '1'})
        with patch.object(linux, 'is_video_capture', return_value=True):
            with self.assertRaisesRegex(RuntimeError, 'Multiple.*XR_CAMERA_DEVICE'):
                runtime.capture_device()
        with patch.object(linux, 'is_video_capture', return_value=False):
            with self.assertRaisesRegex(RuntimeError, 'USB enumeration alone'):
                runtime.capture_device()
        with patch.object(linux, 'is_video_capture', side_effect=PermissionError('permission denied')):
            with self.assertRaisesRegex(RuntimeError, 'permission denied'):
                runtime.capture_device()

    def test_actual_camera_reaches_capture_process_and_release_yaml_is_untouched(self):
        with tempfile.TemporaryDirectory() as folder:
            base = Path(folder)
            runtime = self.runtime({'XR_TCP_PORT': '45810', 'XR_CAMERA_DEVICE': '/dev/video4'})
            runtime.cfg = {}
            runtime.current = base
            runtime.capture_device = lambda: '/dev/video4'
            runtime.generate_profile = lambda: base / 'profile.json'
            launched = []
            runtime.run_managed = lambda args, env, handle: launched.append(env)
            with (base / 'lock').open('w') as handle, patch.object(linux.socket, 'socket'), patch.object(linux.os, 'chdir'):
                runtime._start_locked(handle)
            self.assertEqual(launched[0]['CPP_CAPTURE_VIDEO_DEVICE'], '/dev/video4')
            self.assertFalse((base / 'profile.json').exists())

    def test_prepare_waits_for_dev_node_after_sysfs_announces_the_camera(self):
        runtime = self.runtime({'XR_REQUIRE_CAMERA': '1'})
        runtime.root_required = lambda: None
        runtime.user = 'fixture'
        runtime.cfg = {}
        node = Mock()
        node.is_char_device.side_effect = [False, True, True]
        runtime.device_nodes = lambda: ([node], [node], [])
        with patch.object(linux, 'run') as command, patch.object(linux.time, 'sleep') as wait:
            runtime.prepare()
        wait.assert_called_once_with(.25)
        self.assertTrue(any(call.args[0][0] == 'setfacl' for call in command.call_args_list))

    def test_only_current_run_service_failures_are_reported(self):
        with tempfile.TemporaryDirectory() as folder:
            runtime = self.runtime({})
            runtime.base = Path(folder)
            logs = runtime.base / 'logs'
            logs.mkdir()
            (logs / 'capture_service.log').write_text('[ERROR] video4: camera unavailable\n')
            (logs / 'old.log').write_text('[ERROR] old run\n')
            import os
            os.utime(logs / 'old.log', ns=(1, 1))
            with patch('builtins.print') as output:
                runtime.failure_logs(100)
            text = '\n'.join(str(call.args[0]) for call in output.call_args_list)
            self.assertIn('camera unavailable', text)
            self.assertNotIn('old run', text)


if __name__ == '__main__':
    unittest.main()
