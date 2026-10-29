#!/usr/bin/env python3
"""Exercise the real adapter over loopback, without XR hardware or SHM outputs."""

import argparse
import contextlib
import importlib.util
import json
from pathlib import Path
import socket
import struct
import subprocess
import tempfile
import threading
import time
import unittest
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[3]
ADAPTER = None
NET_HEADER = struct.Struct("<4s4HI5Q2I")
HMD = struct.Struct("<2I4Q13d2IfI")
HAND_HEADER = struct.Struct("<2I4Q2IfI")
HAND_SIDE = struct.Struct("<3If29f4I")
JOINT = struct.Struct("<2I9f")


def free_port(kind=socket.SOCK_STREAM):
    with socket.socket(socket.AF_INET, kind) as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def packet(kind, sequence, timestamp, payload):
    return NET_HEADER.pack(b"XTRK", 1, NET_HEADER.size, kind, 0, len(payload),
                           sequence, sequence, timestamp, timestamp, 0, 0, 0) + payload


def hand_side(side, visible):
    pose = [0.1 * side, 1.0, -0.3, 1.0, 0.0, 0.0, 0.0]
    base = HAND_SIDE.pack(side, int(visible), 9 if visible else 0, float(visible),
                          *(pose * 3 + [0.0] * 6 + [0.0, 0.0]), 0, 0, 21 if visible else 0, 0)
    joints = b"".join(JOINT.pack(i, int(visible), 0.01 * i, 0.2, -0.1, 1.0, 0.0, 0.0, 0.0, 0.008, float(visible))
                      for i in range(21))
    return base + joints


class Source:
    def __init__(self, port):
        self.port = port
        self.visible = True
        self.stop = threading.Event()
        self.thread = threading.Thread(target=self.run)
        self.thread.start()

    def run(self):
        sequence = 0
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
            while not self.stop.is_set():
                sequence += 1
                timestamp = time.monotonic_ns()
                pose = HMD.pack(1, HMD.size, sequence, timestamp, timestamp, 0,
                                0.25, 1.5, -0.75, 1.0, 0.0, 0.0, 0.0, *([0.0] * 6), 2, 1, 1.0, 0)
                sock.sendto(packet(1, sequence * 2, timestamp, pose), ("127.0.0.1", self.port))
                visible = self.visible
                hands = HAND_HEADER.pack(2, 2200, sequence, timestamp, timestamp, 0,
                                         int(visible), 15 if visible else 0, float(visible), 2 if visible else 0)
                hands += hand_side(1, visible) + hand_side(2, visible)
                sock.sendto(packet(5, sequence * 2 + 1, timestamp, hands), ("127.0.0.1", self.port))
                self.stop.wait(1 / 240)

    def close(self):
        self.stop.set()
        self.thread.join(timeout=2)


class Client:
    def __init__(self, port, slow=False):
        self.sock = socket.socket()
        self.sock.settimeout(3)
        if slow:
            self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1024)
        self.sock.connect(("127.0.0.1", port))
        self.stream = self.sock.makefile("rb")

    def read(self):
        line = self.stream.readline(1024 * 1024)
        if not line:
            raise EOFError("disconnected")
        return json.loads(line)

    def frame(self, predicate=lambda _: True):
        deadline = time.monotonic() + 3
        while time.monotonic() < deadline:
            message = self.read()
            if message.get("type") == "frame" and predicate(message):
                return message
        raise AssertionError("no matching frame")

    def close(self, reset=False):
        if reset:
            self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
        self.stream.close()
        self.sock.close()


@contextlib.contextmanager
def runtime(enabled=True, extra=()):
    tcp_port, udp_port = free_port(), free_port(socket.SOCK_DGRAM)
    args = [ADAPTER, "--input", "udp", "--udp-bind-host", "127.0.0.1", "--udp-bind-port", str(udp_port),
            "--mode", "tick", "--tick-rate", "240", "--no-prediction", "--consume-stale-as-lost",
            "--app-tcp-port", str(tcp_port), "--print-every", "0", *extra]
    if enabled:
        args += ["--app-tcp", "--app-tcp-max-clients", "2", "--app-tcp-rate", "240"]
    with tempfile.TemporaryFile(mode="w+") as log:
        process = subprocess.Popen(args, stdout=log, stderr=subprocess.STDOUT)
        source = Source(udp_port)
        try:
            if enabled:
                deadline = time.monotonic() + 5
                while True:
                    try:
                        probe = socket.create_connection(("127.0.0.1", tcp_port), timeout=0.1)
                        probe.close()
                        break
                    except OSError:
                        if process.poll() is not None or time.monotonic() >= deadline:
                            log.seek(0)
                            raise AssertionError(log.read())
                        time.sleep(0.01)
                # Let the worker remove the connection used to probe startup.
                time.sleep(0.03)
            yield process, source, tcp_port
        finally:
            source.close()
            process.terminate()
            try:
                process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
                raise AssertionError("adapter shutdown stalled")
            if process.returncode not in (0, -15):
                log.seek(0)
                raise AssertionError(log.read())


class AppTcpTest(unittest.TestCase):
    def test_simple_cli_and_environment_override_emulation_and_skip_its_profile(self):
        cases=[(["--simple-gestures","--emulation-controller","1"],{}),
               ([],{"SIMPLE_GESTURES":"1"})]
        import os
        for options,env in cases:
            args=[ADAPTER,"--adapter","logging","--input","udp","--udp-bind-host","127.0.0.1",
                  "--udp-bind-port",str(free_port(socket.SOCK_DGRAM)),"--mode","tick","--duration","0.03",
                  "--print-every","0","--emulation-controller-config","/not/a/profile.json",*options]
            result=subprocess.run(args,env=dict(os.environ,SIMPLE_GESTURES="0")|env,
                                  capture_output=True,text=True,timeout=5)
            self.assertEqual(result.returncode,0,result.stderr)
            self.assertIn("simple_gestures: true",result.stdout)
            self.assertIn("emulation_controller: false",result.stdout)

    def test_original_grab_and_pinch_are_selected_instead_of_emulation(self):
        def backend_side(side,visible):
            p=[.1*side,1.,-.3,1.,0.,0.,0.]
            base=HAND_SIDE.pack(side,int(visible),57 if visible else 0,float(visible),
                *(p*3+[0.]*6+[.9,.8]),1,1,21,0)
            return base+b''.join(JOINT.pack(i,2,.01*i,.2,-.1,1,0,0,0,.008,float(visible)) for i in range(21))
        for simple in (False,True):
            with patch(__name__+'.hand_side',backend_side):
                with runtime(extra=["--runtime-derive-hand-gestures","0"]+(["--simple-gestures"] if simple else [])) as (_,_,port):
                    client=Client(port)
                    try:
                        frame=client.frame(lambda f:f.get("controllers") and f.get("hands") and f["hands"]["hand_count"]==2)
                        for name in ("left","right"):
                            control=frame["controllers"][name]
                            self.assertEqual(int(control["buttons"])&3,3 if simple else 0)
                            self.assertAlmostEqual(control["trigger"],.9 if simple else 0,places=4)
                            self.assertAlmostEqual(control["grip"],.8 if simple else 0,places=4)
                    finally:client.close()

    def test_invalid_server_configuration(self):
        for option, value in (("--app-tcp-port", "0"), ("--app-tcp-port", "65536"),
                              ("--app-tcp-rate", "0"), ("--app-tcp-rate", "nan"),
                              ("--app-tcp-max-clients", "0"), ("--app-tcp-max-clients", "33")):
            with self.subTest(option=option, value=value):
                result = subprocess.run([ADAPTER, "--input", "udp", "--udp-bind-host", "127.0.0.1",
                                         "--udp-bind-port", str(free_port(socket.SOCK_DGRAM)),
                                         "--app-tcp", option, value], capture_output=True, text=True, timeout=3)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(option, result.stdout + result.stderr)

    def test_off_by_default(self):
        with runtime(enabled=False, extra=["--duration", "0.25"]) as (process, _, port):
            with socket.socket() as occupied:
                occupied.bind(("127.0.0.1", port))
                occupied.listen()
                self.assertEqual(process.wait(timeout=3), 0)

    def test_multiple_clients_runtime_outputs_loss_and_reset(self):
        with runtime() as (_, source, port):
            first, second = Client(port), Client(port)
            try:
                for client in (first, second):
                    hello = client.read()
                    self.assertEqual(hello["protocol"], "xr_gate_app_v1")
                    self.assertEqual(hello["quaternion_order"], "xyzw")
                    frame = client.frame(lambda f: f["hmd"] and f["hmd"]["pose_valid"] and f["hands"]["hand_count"] == 2)
                    self.assertEqual(frame["hmd"]["position"], [0.25, 1.5, -0.75])
                    self.assertEqual(frame["hmd"]["orientation_xyzw"], [0, 0, 0, 1])
                    self.assertIsInstance(frame["hmd"]["timestamp_ns"], str)
                    self.assertEqual(len(frame["hands"]["left"]["joints"]), 21)
                    self.assertEqual(frame["hands"]["right"]["joints"][20]["joint_id"], 20)
                    self.assertTrue(frame["controllers"]["left"]["pose_valid"])
                    self.assertEqual(frame["controllers"]["left"]["buttons"], "0")
                    self.assertEqual(len(frame["controllers"]["left"]["press_counters"]), 32)
                extra = Client(port)
                try:
                    with self.assertRaises(EOFError):
                        extra.read()
                finally:
                    extra.close()
                source.visible = False
                lost = first.frame(lambda f: f["hands"]["hand_count"] == 0)
                self.assertFalse(lost["hands"]["left"]["pose_valid"])
                self.assertEqual(lost["hands"]["left"]["joints"], [])
                self.assertFalse(lost["controllers"]["left"]["pose_valid"])
                second.close(reset=True)
                second = None
                source.visible = True
                self.assertEqual(first.frame(lambda f: f["hands"]["hand_count"] == 2)["hands"]["hand_count"], 2)
                source.close()
                stale = first.frame(lambda f: f["hmd"] and not f["hmd"]["pose_valid"] and f["hands"]["hand_count"] == 0)
                self.assertFalse(stale["hands"]["right"]["pose_valid"])
            finally:
                first.close()
                if second:
                    second.close()

    def test_slow_client_does_not_stall_fast_client(self):
        with runtime() as (_, _, port):
            fast, slow = Client(port), Client(port, slow=True)
            try:
                self.assertEqual(fast.read()["type"], "hello")
                received, previous, byte_count = 0, 0, 0
                started = time.monotonic()
                while time.monotonic() - started < 1.2:
                    frame = fast.frame()
                    self.assertGreater(frame["sequence"], previous)
                    previous = frame["sequence"]
                    received += 1
                    byte_count += len(json.dumps(frame))
                self.assertGreater(received, 20)
                # A non-reading peer is evicted, freeing the second client slot.
                replacement = Client(port)
                try:
                    self.assertEqual(replacement.read()["type"], "hello")
                    self.assertEqual(replacement.frame()["type"], "frame")
                finally:
                    replacement.close()
                print(f"loopback tracking: {received} frames in {time.monotonic() - started:.2f}s, "
                      f"~{byte_count / received:.0f} JSON bytes/frame", flush=True)
            finally:
                fast.close()
                slow.close()


class VideoClientTest(unittest.TestCase):
    def test_runtime_forwards_video_and_survives_source_restart(self):
        spec = importlib.util.spec_from_file_location("video_forward_client", ROOT / "tools/xr_video_tcp_client.py")
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        listener = socket.socket()
        listener.bind(("127.0.0.1", 0))
        listener.listen()
        listener.settimeout(6)
        output_port = free_port()
        errors = []
        def source():
            try:
                for epoch in (1, 3):
                    peer, _ = listener.accept()
                    with peer:
                        for seq in range(1, 8):
                            stamp = time.monotonic_ns()
                            net = module.NET_HEADER.pack(b"XSVNET1\0", 1, 56, 96, 8, 8, 0, seq, stamp, stamp)
                            header = module.FRAME_HEADER.pack(1, 96, seq, stamp, stamp, stamp, stamp, stamp,
                                                              4, 2, 4, 4, 1, 1, 8, 8, 1, 0)
                            peer.sendall(net + header + bytes([epoch])*8 + bytes([epoch+1])*8)
                            time.sleep(0.04)
                    # Closing the producer connection resets its sequence to 1.
            except (BrokenPipeError, ConnectionResetError):
                # The runtime context closes after the consumer verifies epoch 2.
                pass
            except Exception as exc:
                errors.append(exc)
            finally:
                listener.close()
        worker = threading.Thread(target=source, daemon=True)
        worker.start()
        try:
            with runtime(extra=("--video-input", "tcp", "--video-tcp-host", "127.0.0.1",
                                "--video-tcp-port", str(listener.getsockname()[1]),
                                "--publish-runtime-video-tcp", "--runtime-video-tcp-port", str(output_port))):
                with socket.create_connection(("127.0.0.1", output_port), timeout=4) as client:
                    client.settimeout(4)
                    first = None
                    for _ in range(20):
                        meta, left, right = module.read_frame(client)
                        self.assertEqual(meta["width"], 4)
                        self.assertEqual(len(left), 8)
                        if left == b"\x01"*8:
                            first = meta["sequence"]
                        if left == b"\x03"*8:
                            self.assertEqual(right, b"\x04"*8)
                            self.assertIsNotNone(first)
                            self.assertGreater(meta["sequence"], first)
                            break
                    else:
                        self.fail("runtime did not forward video after backend restart")
        finally:
            worker.join(timeout=6)
        self.assertFalse(worker.is_alive())
        self.assertEqual(errors, [])

    def test_binary_frame_with_fragmented_tcp_reads(self):
        spec = importlib.util.spec_from_file_location("video_client", ROOT / "tools/xr_video_tcp_client.py")
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        left, right = b"\x01" * 8, b"\x02" * 8
        net = module.NET_HEADER.pack(b"XSVNET1\0", 1, 56, 96, 8, 8, 0, 42, 100, 200)
        header = module.FRAME_HEADER.pack(1, 96, 42, 200, 100, 200, 99, 101,
                                          4, 2, 4, 4, 1, 1, 8, 8, 1, 0)
        receiver, sender = socket.socketpair()
        with receiver, sender:
            receiver.settimeout(2)
            data = net + header + left + right
            def send_fragmented():
                for offset in range(0, len(data), 7):
                    sender.sendall(data[offset:offset + 7])
            worker = threading.Thread(target=send_fragmented)
            worker.start()
            try:
                metadata, actual_left, actual_right = module.read_frame(receiver)
                self.assertEqual(metadata["sequence"], 42)
                self.assertEqual(metadata["width"], 4)
                self.assertEqual(actual_left, left)
                self.assertEqual(actual_right, right)
            finally:
                worker.join(timeout=2)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--adapter", required=True)
    args, remaining = parser.parse_known_args()
    ADAPTER = str(Path(args.adapter).resolve())
    unittest.main(argv=[__file__, *remaining])
