#!/usr/bin/env python3
"""Exercise external controller inputs and the existing driver outputs."""

import argparse
import contextlib
import json
import math
import mmap
import socket
import struct
import subprocess
import tempfile
import time
import unittest
from pathlib import Path

from app_tcp_server_test import Client, free_port

ADAPTER = None
INPUT_SIZE = 1432
SIDE_SIZE = 688
TCP_HEADER = struct.Struct("<IHHIIQQ")
POSE = struct.Struct("<13f")
BUTTONS = (1 << 0) | (1 << 6)
RING_HEADER = struct.Struct("<8s7IQ")
RING_SLOT = struct.Struct("<4Q2I88x")


def six_dof_frame(sequence, left_flags=7, right_status=2, timestamp=None, buttons=BUTTONS):
    stamp = time.monotonic_ns() if timestamp is None else timestamp
    data = bytearray(INPUT_SIZE)
    struct.pack_into("<IIQQQ", data, 0, 3, INPUT_SIZE, sequence, stamp, stamp)
    for index, (flags, status) in enumerate(((left_flags, 2), (15, right_status))):
        side = 56 + index * SIDE_SIZE
        struct.pack_into("<4I3Q4f", data, side, status, index + 1, flags, 6,
                         buttons, 8, buttons, .25, .5, -.25, .125)
        struct.pack_into("<Q", data, side + 64, 100 + index)
        struct.pack_into("<I", data, side + 80, 7)
    for x in (-.375, .625):
        data += POSE.pack(x, 1.25, -.75, 0, 0, 0, 1, .5, 0, 0, 0, 2, 0)
    return data


@contextlib.contextmanager
def runtime(mode, profile, *options):
    port = free_port()
    args = [ADAPTER, "--input", "udp", "--udp-bind-port", str(free_port(socket.SOCK_DGRAM)),
            "--mode", mode, "--print-every", "0", "--tracking-transform-config", str(profile),
            "--runtime-jitter-filter", "--simple-gestures",
            "--app-tcp", "--app-tcp-port", str(port), *options]
    with tempfile.TemporaryFile(mode="w+") as log:
        process = subprocess.Popen(args, stdout=log, stderr=subprocess.STDOUT)
        try:
            deadline = time.monotonic() + 4
            while True:
                try:
                    with socket.create_connection(("127.0.0.1", port), timeout=.1):
                        pass
                except ConnectionRefusedError:
                    if process.poll() is not None or time.monotonic() > deadline:
                        log.seek(0)
                        raise AssertionError(log.read())
                    time.sleep(.01)
                else:
                    break
            client = Client(port)
            try:
                assert client.read()["type"] == "hello"
                yield client
            finally:
                client.close()
        finally:
            process.terminate()
            try:
                process.wait(timeout=4)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
                raise
            if process.returncode not in (0, -15):
                log.seek(0)
                raise AssertionError(log.read())


def send_tcp(peer, frame, six_dof=True):
    sequence, stamp = struct.unpack_from("<QQ", frame, 8)
    magic, version = (0x43493631, 1) if six_dof else (0x43495633, 3)
    peer.sendall(TCP_HEADER.pack(magic, version, TCP_HEADER.size, len(frame), 0, sequence, stamp) + frame)


def controller_frame(client, predicate):
    return client.frame(lambda frame: frame.get("controllers") and predicate(frame["controllers"]))["controllers"]


class Controller6DofTest(unittest.TestCase):
    def test_controller_inputs_are_mutually_exclusive(self):
        result = subprocess.run(
            [ADAPTER, "--controller-input-transport", "shm", "--controller-6dof-input", "tcp"],
            capture_output=True, text=True, timeout=3)
        self.assertEqual(result.returncode, 1)
        self.assertIn("mutually exclusive", result.stderr)

    def test_tcp_pose_input_loss_and_reconnect(self):
        with tempfile.TemporaryDirectory() as directory, socket.socket() as producer:
            profile = Path(directory) / "xr_21_joint_hand_viewer_verified.json"
            profile.write_text(json.dumps({"controller_override": {
                "no_imu_controller_orientation": {"left": {"rz_deg": 90}},
                "imu_controller_orientation": {"right": {"orientation_offset": {
                    "rotation_deg": {"rz_deg": -90},
                }}},
            }}))
            producer.bind(("127.0.0.1", 0))
            producer.listen()
            producer.settimeout(3)
            with runtime("event", profile, "--controller-6dof-input", "tcp",
                         "--controller-6dof-tcp-port", str(producer.getsockname()[1]),
                         "--runtime-controller-right-orientation-source", "IMU_OVERRIDE_CONTROLLER_RUNTIME",
                         "--runtime-controller-right-imu-acceleration-integration", "1",
                         "--runtime-controller-imu-gravity-control-file", str(Path(directory) / "missing.json"),
                         "--max-controller-age-ms", "100") as client:
                peer, _ = producer.accept()
                with peer:
                    peer.settimeout(3)
                    self.assertEqual(peer.recv(8, socket.MSG_WAITALL), b"XRARCRP1")
                    # TCP freshness is measured on the receiving host.
                    send_tcp(peer, six_dof_frame(1, timestamp=1))
                    active = controller_frame(client, lambda frame: frame["left"]["pose_valid"])
                    left, right = active["left"], active["right"]
                    self.assertEqual(left["position"], [-.375, 1.25, -.75])
                    self.assertEqual(right["position"], [.625, 1.25, -.75])
                    for side, sign in ((left, 1), (right, -1)):
                        self.assertTrue(side["connected"] and side["pose_valid"] and side["input_valid"])
                        self.assertEqual(side["linear_velocity"], [.5, 0, 0])
                        self.assertEqual(side["angular_velocity"], [0, 2, 0])
                        self.assertAlmostEqual(side["orientation_xyzw"][2], sign * math.sqrt(.5), places=6)
                        self.assertAlmostEqual(side["orientation_xyzw"][3], math.sqrt(.5), places=6)
                        self.assertEqual(int(side["buttons"]), BUTTONS)
                        self.assertEqual(int(side["touches"]), 8)
                        self.assertEqual(side["press_counters"][0], 7)
                        self.assertEqual(side["trigger"], .25)
                        self.assertEqual(side["thumbstick"], [-.25, .125])
                    send_tcp(peer, six_dof_frame(2, left_flags=6, right_status=4))
                    partial = controller_frame(client, lambda frame: not frame["right"]["connected"]
                                               and frame["left"]["input_valid"])
                    self.assertFalse(partial["left"]["pose_valid"])
                    self.assertEqual(int(partial["left"]["buttons"]), BUTTONS)
                    self.assertEqual(int(partial["right"]["buttons"]), 0)
                    expired = controller_frame(client, lambda frame: not frame["left"]["connected"])
                    self.assertFalse(expired["left"]["input_valid"])
                    self.assertEqual(int(expired["left"]["buttons"]), 0)
                peer, _ = producer.accept()
                with peer:
                    peer.settimeout(3)
                    self.assertEqual(peer.recv(8, socket.MSG_WAITALL), b"XRARCRP1")
                    send_tcp(peer, six_dof_frame(1))
                    controller_frame(client, lambda frame: frame["left"]["pose_valid"])
                controller_frame(client, lambda frame: not frame["left"]["connected"])

    def test_shm_input_and_runtime_state_keep_the_binary_driver_contract(self):
        with tempfile.TemporaryDirectory() as directory, tempfile.NamedTemporaryFile(dir="/dev/shm") as source:
            profile = Path(directory) / "xr_21_joint_hand_viewer_verified.json"
            with tempfile.NamedTemporaryFile(dir="/dev/shm") as destination:
                output_name = Path(destination.name).name
            output_path = Path("/dev/shm") / output_name
            self.addCleanup(output_path.unlink, missing_ok=True)
            output_registry = Path(directory) / "runtime_registry.json"
            profile.write_text(json.dumps({"controller_override": {"runtime_controller_state": {
                "publish_shm": True, "registry": str(output_registry), "shm_name": output_name, "slots": 4,
            }}}))
            registry = Path(directory) / "tracking_registry.json"
            payload = six_dof_frame(1)
            header_size = 4096
            source_stride = RING_SLOT.size + len(payload)
            source.truncate(header_size + source_stride)
            registry.write_text(json.dumps({"streams": {"controller_6dof": {
                "format_name": "CONTROLLER_6DOF_INPUT_V1", "format_version": 1,
                "shm_name": Path(source.name).name, "slot_count": 1,
                "payload_size": len(payload), "header_size": header_size,
                "slot_header_size": RING_SLOT.size, "slot_stride": source_stride,
            }}}))
            with mmap.mmap(source.fileno(), 0) as ring:
                RING_HEADER.pack_into(ring, 0, b"HTRKRG1", 1, header_size, 1, source_stride,
                                      RING_SLOT.size, len(payload), 0, 0)
                with runtime("tick", profile, "--controller-6dof-input", "shm",
                             "--controller-6dof-registry", str(registry),
                             "--max-controller-age-ms", "400") as client:
                    payload = six_dof_frame(1)
                    stamp = struct.unpack_from("<Q", payload, 16)[0]
                    RING_SLOT.pack_into(ring, header_size, 2, 2, stamp, stamp, len(payload), 0)
                    ring[header_size + RING_SLOT.size:] = payload
                    struct.pack_into("<Q", ring, RING_HEADER.size - 8, 1)
                    controller_frame(client, lambda frame: frame["left"]["pose_valid"])
                    metadata = json.loads(output_registry.read_text())["streams"]["runtime_controller_state"]
                    self.assertEqual(metadata["format_name"], "RUNTIME_CONTROLLER_STATE_V1")
                    with output_path.open("rb") as output, mmap.mmap(output.fileno(), 0, access=mmap.ACCESS_READ) as data:
                        _, _, header, slots, stride, slot_header, size, _, sequence = RING_HEADER.unpack_from(data)
                        self.assertEqual(size, 936)
                        self.assertGreater(sequence, 0)
                        frame_offset = header + (sequence - 1) % slots * stride + slot_header
                        self.assertEqual(struct.unpack_from("<3I", data, frame_offset), (0x52544331, 1, 936))
                        self.assertEqual(struct.unpack_from("<3f", data, frame_offset + 32 + 40), (-.375, 1.25, -.75))
                        self.assertEqual(struct.unpack_from("<Q", data, frame_offset + 32 + 92)[0], BUTTONS)
                    controller_frame(client, lambda frame: not frame["left"]["pose_valid"])

    def test_legacy_tcp_input_remains_enabled_without_6dof_options(self):
        with tempfile.TemporaryDirectory() as directory, socket.socket() as producer:
            profile = Path(directory) / "xr_21_joint_hand_viewer_verified.json"
            profile.write_text("{}")
            producer.bind(("127.0.0.1", 0))
            producer.listen()
            producer.settimeout(3)
            with runtime("tick", profile, "--controller-input-transport", "tcp",
                         "--controller-input-port", str(producer.getsockname()[1]),
                         "--controller-input-mode", "controller_buttons_runtime_only") as client:
                peer, _ = producer.accept()
                with peer:
                    peer.settimeout(3)
                    self.assertEqual(peer.recv(8, socket.MSG_WAITALL), b"XRARCRP1")
                    send_tcp(peer, six_dof_frame(1)[:INPUT_SIZE], six_dof=False)
                    state = controller_frame(client, lambda frame: frame["left"]["input_valid"])["left"]
                    self.assertEqual(int(state["buttons"]), BUTTONS)
                    self.assertFalse(state["pose_valid"])

    def test_tcp_listener_requires_an_explicit_port(self):
        with tempfile.TemporaryDirectory() as directory:
            profile = Path(directory) / "xr_21_joint_hand_viewer_verified.json"
            profile.write_text("{}")
            result = subprocess.run(
                [ADAPTER, "--input", "udp", "--udp-bind-port", str(free_port(socket.SOCK_DGRAM)),
                 "--mode", "tick", "--duration", ".1", "--tracking-transform-config", str(profile),
                 "--controller-input-transport", "tcp_listen"],
                capture_output=True, text=True, timeout=3)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("controller_input_transport: none\n", result.stdout)
        self.assertNotIn("controller input thread started", result.stdout)

    def test_tcp_listener_receives_v3_after_reconnect(self):
        with tempfile.TemporaryDirectory() as directory:
            profile = Path(directory) / "xr_21_joint_hand_viewer_verified.json"
            profile.write_text("{}")
            port = free_port()
            with runtime("tick", profile, "--controller-input-transport", "tcp_listen",
                         "--controller-input-port", str(port),
                         "--controller-input-mode", "controller_buttons_runtime_only") as client:
                deadline = time.monotonic() + 3
                while True:
                    try:
                        peer = socket.create_connection(("127.0.0.1", port), timeout=1)
                        break
                    except ConnectionRefusedError:
                        if time.monotonic() >= deadline:
                            raise
                        time.sleep(.01)
                for sequence, buttons in enumerate((BUTTONS, 1), start=1):
                    with peer:
                        self.assertEqual(peer.recv(8, socket.MSG_WAITALL), b"XRARCRP1")
                        payload = six_dof_frame(sequence, buttons=buttons)[:INPUT_SIZE]
                        send_tcp(peer, payload, six_dof=False)
                        state = controller_frame(
                            client, lambda frame: int(frame["left"]["buttons"]) == buttons)["left"]
                        self.assertTrue(state["input_valid"])
                    if sequence == 1:
                        peer = socket.create_connection(("127.0.0.1", port), timeout=1)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--adapter", required=True)
    args, remaining = parser.parse_known_args()
    ADAPTER = str(Path(args.adapter).resolve())
    unittest.main(argv=[__file__, *remaining])
