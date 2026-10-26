import ctypes
import importlib.util
import io
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

HELPER = Path(__file__).resolve().parents[1] / "linux/xr_gate_audio.py"
SPEC = importlib.util.spec_from_file_location("xr_gate_audio", HELPER)
audio = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(audio)


class AudioDevicesTest(unittest.TestCase):
    def usb(self, root, card, *, serial="headset-123", port="1-1", capture=False):
        usb = root / "sys/devices" / port
        usb.mkdir(parents=True, exist_ok=True)
        for key, value in {"idVendor": "1234", "idProduct": "abcd", "product": "Generic USB audio", "serial": serial}.items():
            (usb / key).write_text(value)
        interface = usb / (port + ":1.0"); interface.mkdir(exist_ok=True)
        (interface / "bInterfaceNumber").write_text("00")
        name = f"pcmC{card}D0{'c' if capture else 'p'}"
        target = interface / "sound" / f"card{card}" / name; target.mkdir(parents=True)
        link = root / "sys/class/sound" / name; link.parent.mkdir(parents=True, exist_ok=True); link.symlink_to(target)
        proc = root / "proc" / f"card{card}"; proc.mkdir(parents=True, exist_ok=True); (proc / "id").write_text("USB_Audio")
        return link

    def test_replug_and_renumbering_preserve_identity_without_following_another_device(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder); link = self.usb(root, 3)
            saved = audio.playback_devices(root / "sys", root / "proc")[0]["id"]
            link.unlink(); self.usb(root, 7, port="2-6")
            returned = audio.playback_devices(root / "sys", root / "proc")
            self.assertEqual(returned[0]["id"], saved)
            self.assertEqual(audio.resolve_device(saved, returned)["card"], 7)
            self.usb(root, 8, port="1-2", serial="other-headset")
            outputs = audio.playback_devices(root / "sys", root / "proc")
            self.assertEqual(len({d["id"] for d in outputs}), 2)
            with self.assertRaisesRegex(RuntimeError, "not connected"):
                audio.resolve_device(saved, [d for d in outputs if d["id"] != saved])

    def test_serial_less_unique_model_reconnects_and_duplicates_are_not_guessed(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder); link = self.usb(root, 0, serial="")
            saved = audio.playback_devices(root / "sys", root / "proc")[0]["id"]
            link.unlink(); self.usb(root, 4, port="2-5", serial="")
            self.assertEqual(audio.playback_devices(root / "sys", root / "proc")[0]["id"], saved)
            self.usb(root, 6, port="2-7", serial="")
            devices = audio.playback_devices(root / "sys", root / "proc")
            self.assertFalse(devices[0]["available"])
            with self.assertRaisesRegex(RuntimeError, "Identical"):
                audio.resolve_device(saved, devices)

    def test_microphones_and_software_loopback_are_not_outputs(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder); self.usb(root, 1, capture=True)
            self.assertEqual(audio.playback_devices(root / "sys", root / "proc"), [])
            self.usb(root, 2, port="2-2")
            self.assertEqual(len(audio.playback_devices(root / "sys", root / "proc")), 1)
            with self.assertRaises(ValueError): audio.resolve_device("default")


class AudioStreamTest(unittest.TestCase):
    def header(self, **overrides):
        data = {"protocol": audio.PROTOCOL, "format": "s16le", "rate": 48000, "channels": 2}; data.update(overrides)
        return json.dumps(data).encode() + b"\n"

    def test_format_negotiation_and_invalid_headers(self):
        self.assertEqual(audio.read_header(io.BytesIO(self.header())), (48000, 2))
        for overrides in ({"protocol": "unknown"}, {"format": "float"}, {"rate": 0}, {"channels": True}, {"channels": 99}):
            with self.subTest(overrides=overrides), self.assertRaises(ValueError): audio.read_header(io.BytesIO(self.header(**overrides)))
        for data in [b"a" * (audio.MAX_HEADER + 1), b"[]\n", b"null\n"]:
            with self.assertRaises(ValueError): audio.read_header(io.BytesIO(data))

    def test_fragmented_packets_preserve_pcm_and_eof_stops(self):
        class Fragmented(io.BytesIO):
            def read(self, size=-1): return super().read(min(size, 3))
        payloads = [bytes(range(64)), b"\0" * 16]
        stream = Fragmented(b"".join(struct.pack("<I", len(p)) + p for p in payloads))
        class Playback:
            def __init__(self): self.data = []
            def write(self, data): self.data.append(data)
        playback = Playback(); acknowledged = io.BytesIO()
        audio.copy_packets(stream, playback, 2, acknowledged)
        self.assertEqual(playback.data, payloads)
        self.assertEqual(acknowledged.getvalue(), b"".join(struct.pack("<I", len(p)) for p in payloads))
        for packet in (b"\x01", struct.pack("<I", audio.MAX_PACKET + 4), struct.pack("<I", 3) + b"abc", struct.pack("<I", 4) + b"ab"):
            with self.subTest(packet=packet), self.assertRaises(ValueError): audio.copy_packets(io.BytesIO(packet), playback, 2)

    def test_alsa_playback_and_eof_shutdown_without_hardware(self):
        if sys.platform != "linux": self.skipTest("ALSA is Linux-only")
        try: ctypes.CDLL("libasound.so.2")
        except OSError: self.skipTest("ALSA runtime is not installed")
        program = """
import importlib.util,sys
spec=importlib.util.spec_from_file_location('audio',sys.argv[1])
audio=importlib.util.module_from_spec(spec);spec.loader.exec_module(audio)
rate,channels=audio.read_header(sys.stdin.buffer)
with audio.AlsaPlayback('null',rate,channels) as output:
 print('ready',flush=True)
 audio.copy_packets(sys.stdin.buffer,output,channels)
"""
        with subprocess.Popen([sys.executable, "-c", program, str(HELPER)], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE) as process:
            payload = b"\0" * 3840
            stdout, stderr = process.communicate(self.header() + struct.pack("<I", len(payload)) + payload, timeout=5)
            self.assertEqual(process.returncode, 0, stderr.decode()); self.assertEqual(stdout, b"ready\n")


if __name__ == "__main__": unittest.main()
