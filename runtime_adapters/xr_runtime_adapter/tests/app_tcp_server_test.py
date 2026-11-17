#!/usr/bin/env python3
"""Exercise the real adapter over loopback, without XR hardware or SHM outputs."""

import argparse
import contextlib
import importlib.util
import json
from multiprocessing import shared_memory
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
BODY_UDP_HEADER = struct.Struct("<I2H2I2Q")
BODY_SET = struct.Struct("<2I4Q4I")
BODY_TRACKER = struct.Struct("<4I2Q2f2I13f64s")


def free_port(kind=socket.SOCK_STREAM):
    with socket.socket(socket.AF_INET, kind) as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def packet(kind, sequence, timestamp, payload):
    return NET_HEADER.pack(b"XTRK", 1, NET_HEADER.size, kind, 0, len(payload),
                           sequence, sequence, timestamp, timestamp, 0, 0, 0) + payload


def body_packet(sequence, timestamp, trackers):
    payload_size = BODY_SET.size + 32 * BODY_TRACKER.size
    payload = BODY_SET.pack(1, payload_size, sequence, timestamp, timestamp, 0, 1, 7, bool(trackers), len(trackers))
    for tracker_id, stable_hash, status, x, *attributes in trackers:
        role, flags, confidence = attributes or (2, 127, 0.75)
        payload += BODY_TRACKER.pack(0, role, status, flags, stable_hash, 0, confidence, 0.5, 0, 0,
                                     x, 1.0, -0.3, 0.5, -0.5, 0.5, -0.5,
                                     0.125, 0.25, 0.375, -0.125, -0.25, -0.375, tracker_id)
    payload += bytes((32 - len(trackers)) * BODY_TRACKER.size)
    return BODY_UDP_HEADER.pack(0x4254524B, 1, BODY_UDP_HEADER.size, len(payload), 0,
                                sequence, timestamp) + payload


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
        self.hmd_position = (.25, 1.5, -.75)
        self.hmd_quaternion = (1., 0., 0., 0.)
        self.send_head = True
        self.hand_count = 2
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
                                *self.hmd_position, *self.hmd_quaternion, *([0.0] * 6), 2, 1, 1.0, 0)
                if self.send_head:
                    sock.sendto(packet(1, sequence * 2, timestamp, pose), ("127.0.0.1", self.port))
                visible = self.visible
                hands = HAND_HEADER.pack(2, 2200, sequence, timestamp, timestamp, 0,
                                         int(visible), 15 if visible else 0, float(visible), self.hand_count if visible else 0)
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
def runtime(enabled=True, extra=(), default_mode=False, tracking_source=True):
    tcp_port, udp_port = free_port(), free_port(socket.SOCK_DGRAM)
    args = [ADAPTER, "--input", "udp", "--udp-bind-host", "127.0.0.1", "--udp-bind-port", str(udp_port),
            "--mode", "tick", "--tick-rate", "240", "--no-prediction", "--consume-stale-as-lost",
            "--app-tcp-port", str(tcp_port), "--print-every", "0"]
    if not default_mode and "--emulation-controller" not in extra:
        args += ["--emulation-controller", "1"]
    if not default_mode:
        for key,value in (("--simple-gestures-touchpad-radius-m","0.30"),("--simple-gestures-pose-yaw-offset-degrees","0")):
            if key not in extra: args += [key,value]
    args += list(extra)
    if enabled:
        args += ["--app-tcp", "--app-tcp-max-clients", "2", "--app-tcp-rate", "240"]
    with tempfile.TemporaryFile(mode="w+") as log:
        process = subprocess.Popen(args, stdout=log, stderr=subprocess.STDOUT)
        source = Source(udp_port) if tracking_source else None
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
            if source:
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
    def test_body_wrist_threshold_and_output_modes(self):
        wrist_source = 1 << 11
        synthetic_joints = 1 << 6
        role_and_position_valid = (1 << 5) | (1 << 1)
        for fallback, neutral, filter_wrists, options in (
                (True, True, True, []),
                (True, False, False, ["--runtime-controller-body-wrist-neutral-skeleton", "0",
                                      "--runtime-body-tracker-filter-wrists", "0"]),
                (False, False, True, ["--runtime-controller-body-wrist-fallback", "0"])):
            with self.subTest(fallback=fallback, neutral=neutral, filter_wrists=filter_wrists):
                body_port = free_port(socket.SOCK_DGRAM)
                extra = ["--body-trackers-input", "udp", "--body-trackers-udp-bind-host", "127.0.0.1",
                         "--body-trackers-udp-bind-port", str(body_port), "--app-tcp-body-trackers",
                         "--runtime-controller-body-wrist-min-confidence", "0.80", *options]
                with runtime(extra=extra, default_mode=True) as (_, source, port):
                    client = Client(port)
                    try:
                        source.hmd_quaternion = (0.707106781, 0.0, 0.707106781, 0.0)
                        client.frame(lambda f: f["hands"] and f["hands"]["left"]["status"] == 1)
                        source.visible = False
                        client.frame(lambda f: f["hands"] and f["hands"]["left"]["status"] != 1)
                        waist = (b"waist", 1, 2, 0.25)
                        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
                            for seq, confidence in ((1, 0.75), (2, 0.91)):
                                wrist = (b"left-wrist", 2, 2, 0.5, 14, role_and_position_valid, confidence)
                                sock.sendto(body_packet(seq, time.monotonic_ns(), [wrist, waist]),
                                            ("127.0.0.1", body_port))
                                observed = client.frame(lambda f: f.get("body_trackers") and
                                                        f["body_trackers"]["sequence"] >= seq)
                                self.assertEqual([t["role"] for t in observed["body_trackers"]["trackers"]],
                                                 [2] if filter_wrists else [14, 2])
                                if seq == 1 or not fallback:
                                    self.assertEqual(observed["controllers"]["left"]["source_mask"] & wrist_source, 0)
                        if fallback:
                            tracked = client.frame(lambda f: f["controllers"]["left"]["source_mask"] & wrist_source)
                            controller = tracked["controllers"]["left"]
                            hand = tracked["hands"]["left"]
                            self.assertAlmostEqual(controller["orientation_xyzw"][1], 0.707106781, places=5)
                            self.assertEqual(int(controller["buttons"]), 0)
                            self.assertFalse(hand["pinch_active"] or hand["grab_active"])
                            self.assertEqual(bool(hand["flags"] & synthetic_joints), neutral)
                            self.assertEqual(len(hand["joints"]), 21 if neutral else 0)
                            if neutral:
                                self.assertTrue(all(joint["flags"] == 3 for joint in hand["joints"]))
                        source.visible = True
                        restored = client.frame(lambda f: f["hands"]["left"]["status"] == 1)
                        self.assertEqual(restored["controllers"]["left"]["source_mask"] & wrist_source, 0)
                        self.assertAlmostEqual(restored["controllers"]["left"]["position"][0],
                                               restored["hands"]["left"]["controller"]["position"][0], places=5)
                    finally:
                        client.close()

    def test_body_trackers_tcp_opt_in(self):
        tracker = (b"waist", 9007199254740997, 2, 0.25)
        body_port = free_port(socket.SOCK_DGRAM)
        extra = ["--body-trackers-input", "udp", "--body-trackers-udp-bind-host", "127.0.0.1",
                 "--body-trackers-udp-bind-port", str(body_port)]
        with runtime(extra=extra, default_mode=True) as (_, _, port):
            client = Client(port)
            try:
                with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
                    sock.sendto(body_packet(1, time.monotonic_ns(), [tracker]), ("127.0.0.1", body_port))
                self.assertNotIn("body_trackers", client.frame(lambda f: f["hmd"] is not None))
            finally:
                client.close()

    def test_body_only_tcp_with_prediction_and_loss(self):
        body_port = free_port(socket.SOCK_DGRAM)
        extra = ["--body-trackers-input", "udp", "--body-trackers-udp-bind-host", "127.0.0.1",
                 "--body-trackers-udp-bind-port", str(body_port), "--app-tcp-body-trackers",
                 "--runtime-body-tracker-stability-gate", "--runtime-body-tracker-prediction-publish-hz", "10",
                 "--runtime-body-tracker-hold-lost-ms", "80",
                 "--runtime-body-tracker-predict-lost-ms", "300", "--runtime-body-tracker-predicted-status", "stale"]
        with runtime(extra=extra, default_mode=True, tracking_source=False) as (_, _, port):
            client = Client(port)
            try:
                self.assertEqual(client.read()["protocol"], "xr_gate_app_v1")
                tracker = (b"waist", 9007199254740997, 2, 0.25)
                with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
                    timestamps = set()
                    for sequence in range(1, 11):
                        timestamp = time.monotonic_ns()
                        timestamps.add(timestamp)
                        sock.sendto(body_packet(sequence, timestamp, [tracker]), ("127.0.0.1", body_port))
                        time.sleep(0.005)
                    observed = client.frame(lambda f: f.get("body_trackers") and
                                            f["body_trackers"]["trackers"][0]["status"] == 2)
                    self.assertIsNone(observed["hmd"])
                    self.assertIsNone(observed["hands"])
                    body = observed["body_trackers"]
                    self.assertIn(int(body["source_timestamp_ns"]), timestamps)
                    self.assertEqual(body["space"], 3)
                    self.assertEqual(body["trackers"][0]["tracker_id"], "waist")
                    self.assertEqual(body["trackers"][0]["stable_id_hash"], "9007199254740997")
                    self.assertEqual(body["trackers"][0]["orientation_xyzw"], [-0.5, 0.5, -0.5, 0.5])
                    held = client.frame(lambda f: f["body_trackers"]["trackers"][0]["status"] == 4)
                    self.assertGreater(held["body_trackers"]["sequence"], body["sequence"])
                    self.assertIn(int(held["body_trackers"]["source_timestamp_ns"]), timestamps)
                    until_expired = max(timestamps) / 1e9 + 0.40 - time.monotonic()
                    if until_expired > 0:
                        time.sleep(until_expired)
                    lost = (tracker[0], tracker[1], 3, tracker[3])
                    sock.sendto(body_packet(12, time.monotonic_ns(), [lost]), ("127.0.0.1", body_port))
                    client.frame(lambda f: f["body_trackers"]["trackers"][0]["status"] == 3)
                    sock.sendto(body_packet(13, time.monotonic_ns(), []), ("127.0.0.1", body_port))
                    empty = client.frame(lambda f: not f["body_trackers"]["trackers"])
                    self.assertEqual(empty["body_trackers"]["flags"] & 1, 0)
                # A new client sees the latest set, including an empty set.
                second = Client(port)
                try:
                    latest = second.frame()["body_trackers"]
                    self.assertEqual(latest["trackers"], [])
                    self.assertEqual(latest["timestamp_ns"], empty["body_trackers"]["timestamp_ns"])
                finally:
                    second.close()
            finally:
                client.close()

    def test_body_shm_reattach_does_not_refresh_stopped_input(self):
        payload_size = BODY_SET.size + 32 * BODY_TRACKER.size
        stride = 128 + payload_size
        with tempfile.TemporaryDirectory() as tmpdir:
            shm = shared_memory.SharedMemory(create=True, size=4096 + stride)
            try:
                registry = Path(tmpdir) / "tracking.json"
                registry.write_text(json.dumps({"streams": {"body_trackers": {
                    "shm_name": "/" + shm.name, "format_name": "BODY_TRACKER_SET_F32_V1",
                    "header_size": 4096, "slot_count": 1, "slot_header_size": 128,
                    "slot_stride": stride, "payload_size": payload_size,
                }}}))
                timestamp = time.monotonic_ns()
                payload = body_packet(1, timestamp, [(b"waist", 0, 2, 0.25)])[BODY_UDP_HEADER.size:]
                struct.pack_into("<8s7IQ", shm.buf, 0, b"BTRKRG1", 1, 4096, 1, stride, 128, payload_size, 0, 1)
                struct.pack_into("<4Q2I", shm.buf, 4096, 2, 2, timestamp, timestamp, payload_size, 0)
                shm.buf[4096 + 128:] = payload
                extra = ["--body-trackers-input", "shm", "--body-trackers-registry", str(registry),
                         "--body-trackers-reattach-on-stale-ms", "400", "--app-tcp-body-trackers"]
                with runtime(extra=extra, default_mode=True) as (_, _, port):
                    client = Client(port)
                    try:
                        first = client.frame(lambda f: "body_trackers" in f)["body_trackers"]
                        # The source leaves its SHM object and last pose behind.
                        later = client.frame(lambda f: int(f["read_timestamp_ns"]) > timestamp + 1_100_000_000)
                        self.assertEqual(later["body_trackers"]["timestamp_ns"], first["timestamp_ns"])
                        self.assertEqual(later["body_trackers"]["sequence"], first["sequence"])
                    finally:
                        client.close()
            finally:
                shm.close()
                shm.unlink()

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

    def test_simple_invalid_geometry_cannot_bypass_recognizer_with_backend_values(self):
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
                            self.assertEqual(int(control["buttons"])&3,0)
                            self.assertEqual(control["trigger"],0)
                            self.assertEqual(control["grip"],0)
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
