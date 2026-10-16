#!/usr/bin/env python3
"""Exercise the actual C++ TCP readers against fragmented/coalesced TCP sessions."""
import argparse
import copy
import json
import socket
import subprocess
import threading
import time
import unittest

HELLO = {'type': 'hello', 'protocol': 'xr_gate_app_v1', 'quaternion_order': 'xyzw',
         'position_unit': 'metres', 'timestamp_clock': 'server_monotonic'}


def frame(sequence=1):
    # Deliberately unrelated to the client's monotonic clock; keep full uint64 precision.
    stamp = '9007199254740993000'
    pose = {'position': [1, 2, 3], 'orientation_xyzw': [0, 0.6, 0, 0.8]}
    velocity = {'linear_velocity': [0.1, 0.2, 0.3], 'angular_velocity': [0.4, 0.5, 0.6]}
    controller = dict(pose, **velocity, flags=15, source_mask=16, connected=True, pose_valid=True, input_valid=True,
                      source_device_hash='18446744073709551615', last_input_ns=stamp, last_pose_ns=stamp,
                      buttons='9223372036854775809', touches='2', changed_buttons='1', trigger=0.8, grip=0.4,
                      thumbstick=[0.3, -0.2], press_counters=[4294967295] + [0]*31, release_counters=[0]*32)
    hand = dict(velocity, status=1, flags=63, pose_valid=True, confidence=1, controller=pose, palm=pose, wrist=pose,
                pinch_strength=0.7, grab_strength=0.8, pinch_active=True, grab_active=True,
                joints=[dict(pose, position=[i, 0, 0], joint_id=i, flags=1, radius_m=0.01, confidence=1) for i in range(21)])
    return {'type': 'frame', 'sequence': sequence, 'read_timestamp_ns': stamp,
            'hmd': dict(pose, **velocity, sequence=sequence, timestamp_ns=stamp, source_timestamp_ns=stamp,
                        target_timestamp_ns=stamp, source_sequence=123, reset_counter=2,
                        tracking_status=2, flags=7, pose_valid=True, confidence=0.9),
            'hands': {'sequence': sequence, 'timestamp_ns': stamp, 'source_timestamp_ns': stamp, 'reset_counter': 2,
                      'tracking_status': 1, 'flags': 15, 'confidence': 1, 'hand_count': 2, 'left': hand, 'right': hand},
            'controllers': {'sequence': sequence, 'timestamp_ns': stamp, 'flags': 63, 'left': controller, 'right': controller}}


def wire(value):
    return (json.dumps(value, separators=(',', ':')) + '\n').encode()


class Session:
    def __init__(self, handler, duration=850):
        self.listener = socket.socket()
        self.listener.bind(('127.0.0.1', 0))
        self.listener.listen()
        self.listener.settimeout(0.05)
        self.port = self.listener.getsockname()[1]
        self.handler = handler
        self.accepts = 0
        self.stopped = threading.Event()
        self.errors = []
        self.thread = threading.Thread(target=self.serve, daemon=True)
        self.duration = duration

    def serve(self):
        while not self.stopped.is_set():
            try:
                connection, _ = self.listener.accept()
            except socket.timeout:
                continue
            except OSError:
                break
            self.accepts += 1
            try:
                with connection:
                    connection.settimeout(2)
                    self.handler(connection, self.accepts)
            except (BrokenPipeError, ConnectionResetError, socket.timeout):
                pass
            except Exception as error:
                self.errors.append(error)

    def run(self):
        self.thread.start()
        try:
            start = time.monotonic()
            result = subprocess.run([PROBE, str(self.port), str(self.duration)], capture_output=True, text=True, timeout=8)
            assert result.returncode == 0, result.stderr
            assert time.monotonic() - start < self.duration/1000 + 2, 'Shutdown blocked on socket I/O'
            assert not self.errors, self.errors
            return [json.loads(line) for line in result.stdout.splitlines()]
        finally:
            self.stopped.set()
            self.listener.close()
            self.thread.join(timeout=2)


class TcpTest(unittest.TestCase):
    def test_fragmentation_shared_connection_and_exact_contract(self):
        def handler(sock, _):
            data = wire(HELLO) + wire(frame())
            for begin, end in ((0, 3), (3, 51), (51, 1000), (1000, len(data))):
                sock.sendall(data[begin:end]); time.sleep(0.005)
            # Refresh valid distinct snapshots without opening more connections.
            for sequence in range(2, 22):
                sock.sendall(wire(frame(sequence))); time.sleep(0.03)
            time.sleep(0.25)
        session = Session(handler, 700)
        rows = session.run()
        valid = [r for r in rows if r['pose'] and r['hands'] and r['hands2'] and r['controllers'] and r['controllers2'] and r['fresh']]
        self.assertTrue(valid)
        self.assertEqual(session.accepts, 1, 'All five readers must share one TCP connection')
        row = valid[-1]
        self.assertTrue(row['fresh'])
        self.assertEqual(row['position'], [1, 2, 3])
        self.assertEqual(row['quaternion'], [0, 0.6, 0, 0.8])
        self.assertEqual(row['source_hash'], '18446744073709551615')
        self.assertEqual(row['left_buttons'], '9223372036854775809')
        self.assertEqual(row['right_buttons'], row['left_buttons'])
        self.assertEqual(row['press_counter'], 4294967295)
        self.assertEqual(row['roles'], [1, 2])
        self.assertEqual((row['joints'], row['right_joints'], row['last_joint_x']), (21, 21, 20))

    def test_quiet_connection_and_replayed_frames_go_stale(self):
        def handler(sock, _):
            sock.sendall(wire(HELLO) + wire(frame()))
            for _ in range(15):
                time.sleep(0.03); sock.sendall(wire(frame()))
        rows = Session(handler, 400).run()
        self.assertTrue(any(r['fresh'] for r in rows))
        self.assertTrue(any(r['pose'] and not r['fresh'] and r['ms'] > 200 for r in rows))

    def test_disconnect_clears_all_streams_and_reconnect_resets_sequence(self):
        def handler(sock, index):
            if index == 2:
                time.sleep(0.15)  # the disconnected cache must remain empty during hello wait
            sock.sendall(wire(HELLO) + wire(frame(100 if index == 1 else 1)))
            time.sleep(0.1 if index == 1 else 0.7)
        session = Session(handler, 650)
        rows = session.run()
        self.assertTrue(any(r['sequence'] == 100 for r in rows))
        self.assertTrue(any(r['sequence'] == 1 and r['fresh'] for r in rows))
        empty = [r for r in rows if 150 < r['ms'] < 220]
        self.assertTrue(empty)
        self.assertTrue(all(not r['pose'] and not r['hands'] and not r['controllers'] and r['left_buttons'] == '0' for r in empty))
        self.assertEqual(session.accepts, 2)

    def test_wrong_protocol_and_oversized_line_are_rejected_then_recovered(self):
        def handler(sock, index):
            if index == 1:
                sock.sendall(wire(dict(HELLO, protocol='wrong')))
            elif index == 2:
                sock.sendall(wire(HELLO) + b'x' * (1024*1024 + 1))
            else:
                sock.sendall(wire(HELLO) + wire(frame()))
            time.sleep(0.15)
        session = Session(handler, 750)
        rows = session.run()
        self.assertGreaterEqual(session.accepts, 3)
        self.assertTrue(any(r['pose'] for r in rows))

    def test_null_streams_and_nonfinite_pose_do_not_keep_valid_old_data(self):
        def handler(sock, _):
            first = frame(); broken = copy.deepcopy(first)
            broken['sequence'] = 2; broken['hmd']['position'][0] = None
            sock.sendall(wire(HELLO) + wire(first))
            time.sleep(0.08); sock.sendall(wire(broken))
            time.sleep(0.08); sock.sendall(wire(dict(frame(3), hmd=None, hands=None, controllers=None)))
            time.sleep(0.5)
        rows = Session(handler, 400).run()
        self.assertTrue(any(r['pose'] and not (r['pose_flags'] & 1) for r in rows))
        self.assertTrue(all(not r['pose'] and not r['hands'] and not r['controllers'] for r in rows if r['ms'] > 250))

    def test_no_server_does_not_block_frame_reads_or_shutdown(self):
        listener = socket.socket(); listener.bind(('127.0.0.1', 0))
        port = listener.getsockname()[1]; listener.close()
        result = subprocess.run([PROBE, str(port), '200'], capture_output=True, text=True, timeout=2)
        self.assertEqual(result.returncode, 0, result.stderr)
        rows = [json.loads(line) for line in result.stdout.splitlines()]
        self.assertGreater(len(rows), 8)
        self.assertTrue(all(not r['pose'] and not r['controllers'] for r in rows))

    def test_source_age_uses_server_clock_delta(self):
        def handler(sock, _):
            old = frame()
            old['read_timestamp_ns'] = str(int(old['hmd']['timestamp_ns']) + 500_000_000)
            sock.sendall(wire(HELLO) + wire(old))
            time.sleep(0.4)
        rows = Session(handler, 200).run()
        poses = [r for r in rows if r['pose']]
        self.assertTrue(poses)
        self.assertTrue(all(not r['fresh'] and r['age_ns'] >= 500_000_000 for r in poses))

    def test_malformed_counter_array_disconnects_and_recovers(self):
        def handler(sock, index):
            value = frame()
            if index == 1:
                value['controllers']['left']['press_counters'] = [0]
            sock.sendall(wire(HELLO) + wire(value))
            time.sleep(0.2)
        session = Session(handler, 450)
        rows = session.run()
        self.assertGreaterEqual(session.accepts, 2)
        self.assertTrue(any(r['pose'] and r['controllers'] for r in rows))

    def test_press_release_between_snapshots_is_delivered_once_with_counter_wrap(self):
        def handler(sock, _):
            first = frame()
            for side in ('left', 'right'):
                first['controllers'][side].update(buttons='0', trigger=0, grip=0,
                    press_counters=[4294967295] + [0]*31, release_counters=[7] + [0]*31)
            sock.sendall(wire(HELLO) + wire(first))
            time.sleep(0.1)
            second = copy.deepcopy(first); second['sequence'] = 2
            for side in ('left', 'right'):
                second['controllers'][side]['press_counters'][0] = 0
                second['controllers'][side]['release_counters'][0] = 8
            sock.sendall(wire(second))
            time.sleep(0.3)
        rows = Session(handler, 300).run()
        self.assertEqual(sum(r['left_buttons'] == '1' for r in rows), 1)
        self.assertEqual(sum(r['right_buttons'] == '1' for r in rows), 1)
        self.assertTrue(all(r['left_buttons'] == '0' for r in rows if r['ms'] > 180))

    def test_reset_counters_are_not_replayed_as_new_presses(self):
        def handler(sock, _):
            first = frame()
            for side in ('left', 'right'):
                first['controllers'][side].update(buttons='0', trigger=0, grip=0,
                    press_counters=[7] + [0]*31, release_counters=[7] + [0]*31)
            sock.sendall(wire(HELLO) + wire(first))
            time.sleep(0.1)
            second = copy.deepcopy(first); second['sequence'] = 2
            for side in ('left', 'right'):
                second['controllers'][side]['press_counters'][0] = 0
                second['controllers'][side]['release_counters'][0] = 0
            sock.sendall(wire(second)); time.sleep(0.3)
        rows = Session(handler, 300).run()
        self.assertTrue(any(r['controllers'] for r in rows))
        self.assertTrue(all(r['left_buttons'] == '0' and r['right_buttons'] == '0' for r in rows))


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--probe', required=True)
    args, rest = parser.parse_known_args()
    PROBE = args.probe
    unittest.main(argv=[__file__, *rest])
