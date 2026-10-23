import importlib.util
import json
import os
from pathlib import Path
import select
import subprocess
import sys
import tempfile
import time
import unittest

HELPER = Path(__file__).resolve().parents[1] / 'linux/runtime_progress.py'
spec = importlib.util.spec_from_file_location('runtime_progress', HELPER)
progress = importlib.util.module_from_spec(spec)
spec.loader.exec_module(progress)


def sample(side, flags=367, connected='yes'):
    return f'controller_{side}_connected={connected} controller_{side}_imu_data_flags={flags}\n'


def alignment(side, yaw='yes', translation='yes'):
    return (f'[imu-alignment] controller_{side}_acceleration_yaw_valid={yaw} '
            f'controller_{side}_acceleration_translation_yaw_calibrated={translation}\n')


class CalibrationTest(unittest.TestCase):
    def test_only_assigned_enabled_imu_sides_are_required(self):
        devices = [dict(id=1, backend='gearvr_ble', imu_side='left', phys='AA:BB'),
                   dict(id=2, backend='gearvr_ble', imu_side='right', stable_hash='00000000'),
                   dict(id=3, backend='gearvr_ble', imu_side='right', phys='AA:CC', enabled=False),
                   dict(id=4, backend='evdev', imu_side='right', phys='input'),
                   dict(id=5, backend='gearvr_ble', imu_side='right', stable_hash=None)]
        self.assertEqual(progress.imu_sides(dict(devices=devices)), {'left'})
        devices.append(dict(id=6, backend='gearvr_ble', stable_hash=123))
        self.assertEqual(progress.imu_sides(dict(devices=devices, bindings=[dict(device_id=6, side='right')])), {'left', 'right'})

    def test_single_side_zeroing_and_optical_stages_without_spam(self):
        monitor = progress.CalibrationProgress({'left'})
        self.assertEqual(monitor.begin(), ['zeroing'])
        self.assertEqual(monitor.feed(sample('left', flags=355)), [])
        self.assertEqual(monitor.feed(sample('left', connected='no')), [])
        self.assertEqual(monitor.feed(sample('left')), ['optical'])
        self.assertEqual(monitor.feed(sample('left')), [])
        self.assertEqual(monitor.feed(alignment('left', translation='no')), [])
        self.assertEqual(monitor.feed(alignment('left')), ['complete'])
        self.assertEqual(monitor.feed(alignment('left')), [])

    def test_both_sides_need_fresh_zero_and_alignment_after_reset(self):
        monitor = progress.CalibrationProgress({'left', 'right'})
        monitor.begin()
        self.assertEqual(monitor.feed(sample('left')), [])
        self.assertEqual(monitor.feed(sample('right')), ['optical'])
        monitor.feed(alignment('left'))
        monitor.feed('controller IMU state reset: epoch changed\n')
        self.assertEqual(monitor.feed(alignment('right')), [])
        monitor.feed(sample('left'))
        self.assertEqual(monitor.feed(sample('right')), [])
        self.assertEqual(monitor.feed(alignment('left').strip() + ' ' + alignment('right')), ['complete'])

    def test_disabled_translation_and_gestures(self):
        monitor = progress.CalibrationProgress({'right'})
        monitor.begin()
        monitor.feed('[imu-calibration] side=right yaw=collecting translation=disabled')
        monitor.feed(sample('right'))
        self.assertEqual(monitor.feed(alignment('right', translation='no')), ['complete'])
        gestures = progress.CalibrationProgress(set())
        self.assertEqual(gestures.begin() + gestures.feed(sample('left')), [])

    @unittest.skipIf(os.name == 'nt', 'POSIX pipe selector fixture; helper runs in Ubuntu')
    def test_worker_rejects_previous_session_and_exits_on_pipe_eof(self):
        with tempfile.TemporaryDirectory() as folder:
            base = Path(folder)
            (base / 'config').mkdir()
            (base / 'logs').mkdir()
            (base / 'config/override_controller.json').write_text(json.dumps(dict(devices=[
                dict(id=1, backend='gearvr_ble', imu_side='left', phys='AA:BB')])) )
            log = base / 'logs/xr_runtime_adapter.log'
            log.write_text(sample('left') + alignment('left'))
            old_time = time.time_ns() - 10_000_000_000
            os.utime(log, ns=(old_time, old_time))
            token = 'a' * 32
            session = base / 'runtime-session.json'
            session.write_text(json.dumps(dict(session_kind='runtime', launch_token='b' * 32, started_ns=old_time)))
            child = subprocess.Popen([sys.executable, str(HELPER), '--base', folder, '--launch-token', token],
                                     stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, bufsize=0)
            try:
                self.assertEqual(child.stdout.readline(), b'{"stage": "zeroing"}\n')
                self.assertFalse(select.select([child.stdout], [], [], .35)[0], 'Previous session emitted calibration success')
                started_ns = time.time_ns()
                session.write_text(json.dumps(dict(session_kind='runtime', launch_token=token, started_ns=started_ns)))
                self.assertFalse(select.select([child.stdout], [], [], .35)[0], 'Previous log emitted calibration success')
                log.write_text(sample('left') + alignment('left'))
                for stage in ('optical', 'complete'):
                    self.assertTrue(select.select([child.stdout], [], [], 3)[0], 'Current calibration was not reported')
                    self.assertEqual(json.loads(child.stdout.readline()), {'stage': stage})
                child.stdin.close()
                # EOF must stop promptly even with a pending/new log.
                child.wait(timeout=3)
                self.assertEqual(child.returncode, 0, child.stderr.read().decode())
            finally:
                if child.poll() is None:
                    child.kill()
                    child.wait(timeout=3)
                if not child.stdin.closed:
                    child.stdin.close()
                child.stdout.close()
                child.stderr.close()


if __name__ == '__main__':
    unittest.main()
