"""Exercise capture inputs through real TCP sockets and the output rings."""

import json
import socket
import struct
import subprocess
import sys
import tempfile
import textwrap
import time
import unittest
import uuid
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from capture_client import CaptureClient


EXE = sys.argv.pop(1)
HEADER = struct.Struct("<4sHHI")
CLOCK_SHIFT = 7_000_000_000


def message(kind, body):
    return HEADER.pack(b"XRCI", 1, kind, len(body)) + body


def receive_exact(peer, size):
    data = bytearray()
    while len(data) < size:
        part = peer.recv(size - len(data))
        if not part:
            raise EOFError("sender socket closed")
        data.extend(part)
    return data


def view(name):
    return {"id": name, "width": 2, "height": 2, "format": "gray8"}


def hello(peer, role, session=None, views=None, imu=False):
    session = session or str(uuid.uuid4())
    data = {"role": role, "session_id": session}
    if views is not None:
        data["views"] = views
    if role == "primary":
        data["has_imu"] = imu
    peer.sendall(message(1, json.dumps(data).encode()))
    while True:
        magic, version, kind, size = HEADER.unpack(receive_exact(peer, HEADER.size))
        assert magic == b"XRCI" and version == 1
        if kind == 6:
            assert size == 0
            return session
        assert kind == 4 and size == 8
        cookie = struct.unpack("<Q", receive_exact(peer, size))[0]
        t1 = time.monotonic_ns() + CLOCK_SHIFT
        t2 = time.monotonic_ns() + CLOCK_SHIFT
        peer.sendall(message(5, struct.pack("<QQQ", cookie, t1, t2)))


class TcpCaptureTest(unittest.TestCase):
    def setUp(self):
        folder = tempfile.TemporaryDirectory()
        self.addCleanup(folder.cleanup)
        self.root = Path(folder.name)
        self.registry = self.root / "main.json"
        self.extra_registry = self.root / "extra.json"
        self.log = self.root / "capture.log"
        self.namespace = "tcp_test_" + uuid.uuid4().hex[:10]
        sockets = [socket.socket() for _ in range(3)]
        try:
            for sock in sockets:
                sock.bind(("127.0.0.1", 0))
            self.camera_port, self.imu_port, self.external_port = [sock.getsockname()[1] for sock in sockets]
        finally:
            for sock in sockets:
                sock.close()

    def wait_for(self, predicate, description):
        deadline = time.monotonic() + 4.0
        while time.monotonic() < deadline and self.proc.poll() is None:
            result = predicate()
            if result:
                return result
            time.sleep(0.01)
        self.fail(f"{description}\n{self.log.read_text(errors='replace')}")

    def start(self, camera, imu, external):
        config = self.root / "main.yaml"
        extra = self.root / "extra.yaml"
        config.write_text(f"""service:
  namespace: {self.namespace}
  registry_path: {self.registry}
  slot_count: 8
  publish: [shm]
camera:
{textwrap.indent(camera, '  ')}
imu:
{textwrap.indent(imu, '  ')}
""")
        extra.write_text(f"""service:
  namespace: {self.namespace}_extra
  registry_path: {self.extra_registry}
  publish: [shm]
cameras:
  room:
{textwrap.indent(external, '    ')}
""")
        with self.log.open("w") as log:
            self.proc = subprocess.Popen([EXE, "--config", config, "--extra-config", extra],
                                         stdout=log, stderr=subprocess.STDOUT)
        self.addCleanup(self.stop)

    def stop(self):
        if self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait()

    def connect(self, port):
        def attempt():
            try:
                return socket.create_connection(("127.0.0.1", port), timeout=1)
            except ConnectionRefusedError:
                return None

        peer = self.wait_for(attempt, f"no listener on {port}")
        peer.settimeout(2)
        self.addCleanup(peer.close)
        return peer

    def client(self, registry):
        self.wait_for(registry.is_file, f"missing registry {registry}")
        client = CaptureClient.from_shm_registry(registry)
        self.addCleanup(client.close)
        return client

    def tcp_camera(self, port, layout):
        return f"""driver: tcp
layout: {layout}
tcp:
  port: {port}
primary:
  width: 2
  height: 2
output:
  width: 2
  height: 2"""

    def local_camera(self, port, stereo):
        width = 4 if stereo else 2
        layout = "side_by_side_horizontal" if stereo else "mono"
        row = bytes([24, 24, 48, 48] if stereo else [24, 24])
        for index in range(4):
            (self.root / f"frame{index:03}.pgm").write_bytes(f"P5\n{width} 2\n255\n".encode() + row * 2)
        return f"""driver: opencv
layout: {layout}
tcp:
  port: {port}
primary:
  api: any
  device: {self.root}/frame%03d.pgm
output:
  width: 2
  height: 2"""

    def test_stereo_additional_optional_imu_and_reconnect(self):
        camera = "stall_exit_ms: 800\n" + self.tcp_camera(self.camera_port, "stereo").replace(
            "primary:", "  views:\n    hand:\n      width: 2\n      height: 2\nprimary:"
        ) + "\n  additional_cameras:\n    hand:\n      source: hand"
        self.start(camera, f"driver: tcp\nstall_exit_ms: 800\ntcp:\n  port: {self.imu_port}",
                   self.tcp_camera(self.external_port, "mono"))
        main = self.client(self.registry)
        extra = self.client(self.extra_registry)
        additional = self.client(self.root / "main_additional.json")

        ext = self.connect(self.external_port)
        hello(ext, "external", views=[view("mono")])
        ext.sendall(message(2, struct.pack("<QQ", 1, time.monotonic_ns() + CLOCK_SHIFT) + b"room"))
        self.wait_for(lambda: extra.read_latest_image("room"), "external waits for primary")
        # The primary and optional IMU listeners outlive the connected-source timeout.
        time.sleep(0.9)

        primary = self.connect(self.camera_port)
        primary.sendall(message(1, json.dumps({"role": "primary", "session_id": str(uuid.uuid4()),
                       "has_imu": False, "views": [view("left"), view("right")]}).encode()))
        self.assertEqual(primary.recv(1), b"")  # Missing the configured hand view.
        primary.close()

        views = [view("left"), view("right"), view("hand")]
        primary = self.connect(self.camera_port)
        hello(primary, "primary", views=views)
        timestamp = time.monotonic_ns() + CLOCK_SHIFT
        group = message(2, struct.pack("<QQ", 1, timestamp) + b"leftrghthand")
        primary.sendall(group[:7])
        primary.sendall(group[7:])
        left = self.wait_for(lambda: main.read_latest_image("camera0"), "missing left image")
        right = self.wait_for(lambda: main.read_latest_image("camera1"), "missing right image")
        hand = self.wait_for(lambda: additional.read_latest_image("hand"), "missing additional image")
        self.assertEqual((left.data, right.data, hand.data), (b"left", b"rght", b"hand"))
        self.assertEqual(left.timestamp_ns, right.timestamp_ns)
        self.assertEqual(left.timestamp_ns, hand.timestamp_ns)
        self.assertLess(abs(left.timestamp_ns - timestamp + CLOCK_SHIFT), 50_000_000)
        self.assertIs(json.loads(self.registry.read_text())["tcp_input"]["imu_available"], False)
        self.assertIsNone(main.read_latest_imu())

        truncated = message(2, struct.pack("<QQ", 2, time.monotonic_ns() + CLOCK_SHIFT) + b"bad1bad2bad3")
        primary.sendall(truncated[:-1])
        primary.close()
        primary = self.connect(self.camera_port)
        session = hello(primary, "primary", views=views, imu=True)
        self.assertEqual(main.latest_sequence("camera0"), 1)
        imu = self.connect(self.imu_port)
        hello(imu, "imu", session=session)
        timestamp = time.monotonic_ns() + CLOCK_SHIFT
        imu.sendall(message(3, struct.pack("<QQ6f", 1, timestamp, 1, 2, 3, 4, 5, 6)))
        primary.sendall(message(2, struct.pack("<QQ", 1, timestamp) + b"new1new2new3"))
        sample = self.wait_for(main.read_latest_imu, "missing IMU")
        self.wait_for(lambda: main.latest_sequence("camera0") == 2, "video did not reconnect")
        self.assertEqual(sample.timestamp_ns, main.read_latest_image("camera0").timestamp_ns)
        self.assertEqual(sample.gyro_rad_s, (1, 2, 3))
        self.assertEqual(sample.accel_m_s2, (4, 5, 6))
        imu.close()
        primary.sendall(message(2, struct.pack("<QQ", 2, time.monotonic_ns() + CLOCK_SHIFT) + b"lefthelpnext"))
        self.wait_for(lambda: main.latest_sequence("camera0") == 3, "IMU disconnect stopped video")

    def test_local_primary_and_tcp_external(self):
        self.start(self.local_camera(self.camera_port, stereo=True),
                   f"enabled: false\ndriver: tcp\ntcp:\n  port: {self.imu_port}",
                   self.tcp_camera(self.external_port, "mono"))
        main = self.client(self.registry)
        self.wait_for(lambda: main.read_latest_image("camera0"), "missing local stereo")
        for port in (self.camera_port, self.imu_port):
            with self.assertRaises(ConnectionRefusedError):
                socket.create_connection(("127.0.0.1", port), timeout=1)
        peer = self.connect(self.external_port)
        hello(peer, "external", views=[view("mono")])
        peer.sendall(message(2, struct.pack("<QQ", 1, time.monotonic_ns() + CLOCK_SHIFT) + b"room"))
        extra = self.client(self.extra_registry)
        frame = self.wait_for(lambda: extra.read_latest_image("room"), "missing TCP external frame")
        self.assertEqual(frame.data, b"room")

    def test_tcp_primary_and_local_external(self):
        self.start(self.tcp_camera(self.camera_port, "stereo"), "enabled: false",
                   self.local_camera(self.external_port, stereo=False))
        extra = self.client(self.extra_registry)
        self.wait_for(lambda: extra.read_latest_image("room"), "missing local external camera")
        with self.assertRaises(ConnectionRefusedError):
            socket.create_connection(("127.0.0.1", self.external_port), timeout=1)
        peer = self.connect(self.camera_port)
        hello(peer, "primary", views=[view("left"), view("right")])
        peer.sendall(message(2, struct.pack("<QQ", 1, time.monotonic_ns() + CLOCK_SHIFT) + b"leftrght"))
        main = self.client(self.registry)
        self.wait_for(lambda: main.read_latest_image("camera0"), "missing TCP stereo")


if __name__ == "__main__":
    unittest.main()
