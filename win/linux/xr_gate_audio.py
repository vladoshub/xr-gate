#!/usr/bin/env python3
"""Generic ALSA playback over a private wsl.exe stdin; no microphone or ports.

Offer hardware outputs only. WSLg/PulseAudio routes back to Windows and would
otherwise create a feedback loop with WASAPI loopback capture.
"""
import argparse
import ctypes
import errno
import hashlib
import json
from pathlib import Path
import re
import signal
import struct
import sys
import time

PROTOCOL = "xr-gate-audio/1"
MAX_HEADER = 4096
MAX_PACKET = 262144


def read_text(path, fallback=""):
    try:
        return path.read_text(encoding="utf-8", errors="replace").strip()
    except OSError:
        return fallback


def playback_devices(sysroot=Path("/sys"), procroot=Path("/proc/asound")):
    devices = {}
    for node in sorted((sysroot / "class/sound").glob("pcmC*D*p")):
        match = re.fullmatch(r"pcmC(\d+)D(\d+)p", node.name)
        if not match:
            continue
        card, pcm = map(int, match.groups())
        resolved = node.resolve()
        usb = next((p for p in (resolved, *resolved.parents) if (p / "idVendor").is_file()), None)
        card_name = read_text(procroot / f"card{card}/id", "ALSA output")
        info = read_text(procroot / f"card{card}/pcm{pcm}p/info")
        pcm_name = next((line[5:].strip() for line in info.splitlines() if line.startswith("name:")), "")
        if usb is not None:
            vid, pid = read_text(usb / "idVendor").lower(), read_text(usb / "idProduct").lower()
            serial, product = read_text(usb / "serial"), read_text(usb / "product", card_name)
            interface = next((read_text(p / "bInterfaceNumber") for p in resolved.parents
                              if (p / "bInterfaceNumber").is_file()), "")
            # Do not persist ALSA card indices, USB bus numbers or vhci ports.
            identity = ["usb", vid, pid, serial, product if not serial else "", interface, pcm]
            name = product
            detail = f"USB {vid}:{pid}" + (f" / {serial}" if serial else "")
        else:
            identity = ["alsa", str(resolved).split("/sound/card", 1)[0], card_name, pcm]
            name, detail = card_name, "ALSA"
        digest = hashlib.sha256(json.dumps(identity, separators=(",", ":")).encode()).hexdigest()
        suffix = f" — {pcm_name}" if pcm_name and pcm_name != name else ""
        item = {"id": "alsa:" + digest, "name": name + suffix + f" [{detail}; PCM {pcm}]",
                "available": True, "card": card, "pcm": pcm}
        if item["id"] in devices:
            # Indistinguishable devices must not silently change the destination.
            devices[item["id"]]["available"] = False
            devices[item["id"]]["reason"] = "Identical devices without a unique serial; disconnect the other device."
        else:
            devices[item["id"]] = item
    return list(devices.values())


def resolve_device(identity, devices=None):
    if not re.fullmatch(r"alsa:[0-9a-f]{64}", identity):
        raise ValueError("Select an ALSA playback device in Settings, or choose No audio.")
    matches = [d for d in (playback_devices() if devices is None else devices) if d["id"] == identity]
    if not matches:
        raise RuntimeError("The selected audio output is not connected; waiting for that same device.")
    if not matches[0]["available"]:
        raise RuntimeError(matches[0].get("reason", "The selected audio output is unavailable."))
    return matches[0]


def read_header(stream):
    line = stream.readline(MAX_HEADER + 1)
    if not line or len(line) > MAX_HEADER or not line.endswith(b"\n"):
        raise ValueError("Missing or oversized audio header.")
    header = json.loads(line)
    if not isinstance(header, dict) or header.get("protocol") != PROTOCOL or header.get("format") != "s16le":
        raise ValueError("Unsupported audio stream protocol/format.")
    rate, channels = header.get("rate"), header.get("channels")
    if type(rate) is not int or not 8000 <= rate <= 192000 or type(channels) is not int or not 1 <= channels <= 8:
        raise ValueError("Invalid audio sample rate/channel count.")
    return rate, channels


def read_exact(stream, count, *, eof=False):
    result = bytearray()
    while len(result) < count:
        block = stream.read(count - len(result))
        if not block:
            if eof and not result:
                return None
            raise ValueError("Truncated audio packet.")
        result.extend(block)
    return bytes(result)


def copy_packets(stream, playback, channels, acknowledge=None):
    while True:
        prefix = read_exact(stream, 4, eof=True)
        if prefix is None:
            return
        size = struct.unpack("<I", prefix)[0]
        if size > MAX_PACKET or size % (channels * 2):
            raise ValueError("Invalid audio packet size/alignment.")
        if size:
            playback.write(read_exact(stream, size))
        if acknowledge is not None:
            acknowledge.write(prefix)
            acknowledge.flush()


class AlsaPlayback:
    def __init__(self, name, rate, channels, *, latency_us=50000):
        self.handle = ctypes.c_void_p()
        self.alignment = channels * 2
        try:
            self.lib = ctypes.CDLL("libasound.so.2")
        except OSError as error:
            raise RuntimeError("ALSA is unavailable. Run Install / continue to install the Ubuntu audio dependency.") from error
        lib = self.lib
        lib.snd_pcm_open.argtypes = [ctypes.POINTER(ctypes.c_void_p), ctypes.c_char_p, ctypes.c_int, ctypes.c_int]
        lib.snd_pcm_set_params.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int, ctypes.c_uint,
                                         ctypes.c_uint, ctypes.c_int, ctypes.c_uint]
        lib.snd_pcm_writei.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_ulong]
        lib.snd_pcm_writei.restype = ctypes.c_long
        for function in ("snd_pcm_prepare", "snd_pcm_drop", "snd_pcm_close"):
            getattr(lib, function).argtypes = [ctypes.c_void_p]
        lib.snd_pcm_wait.argtypes = [ctypes.c_void_p, ctypes.c_int]
        lib.snd_strerror.argtypes = [ctypes.c_int]
        lib.snd_strerror.restype = ctypes.c_char_p
        try:
            self.check(lib.snd_pcm_open(ctypes.byref(self.handle), name.encode(), 0, 1))
            # S16_LE / RW_INTERLEAVED. plughw converts to hardware capabilities.
            self.check(lib.snd_pcm_set_params(self.handle, 2, 3, channels, rate, 1, latency_us))
        except BaseException:
            self.close()
            raise

    def check(self, code):
        if code < 0:
            raise RuntimeError("ALSA playback: " + self.lib.snd_strerror(code).decode(errors="replace"))
        return code

    def write(self, payload):
        data = ctypes.create_string_buffer(payload)
        offset, progress = 0, time.monotonic()
        while offset < len(payload):
            written = self.lib.snd_pcm_writei(self.handle, ctypes.byref(data, offset), (len(payload) - offset) // self.alignment)
            if written > 0:
                offset += written * self.alignment
                progress = time.monotonic()
            elif written == -errno.EPIPE:
                self.check(self.lib.snd_pcm_prepare(self.handle))
            elif written in (0, -errno.EAGAIN, -errno.EINTR):
                self.lib.snd_pcm_wait(self.handle, 50)
            else:
                self.check(written)
            if time.monotonic() - progress > 2:
                raise RuntimeError("Audio output stopped accepting samples.")

    def close(self):
        if self.handle.value:
            self.lib.snd_pcm_drop(self.handle)
            self.lib.snd_pcm_close(self.handle)
            self.handle = ctypes.c_void_p()

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()


def play(stream, output, identity, acknowledge=None):
    rate, channels = read_header(stream)
    device = resolve_device(identity)
    with AlsaPlayback(f"plughw:CARD={device['card']},DEV={device['pcm']}", rate, channels) as playback:
        output.write(json.dumps({"protocol": PROTOCOL, "ready": True, "device": device["name"]}) + "\n")
        output.flush()
        copy_packets(stream, playback, channels, acknowledge)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("list")
    playback = commands.add_parser("play")
    playback.add_argument("--device", required=True)
    args = parser.parse_args()
    if args.command == "list":
        print(json.dumps({"protocol": PROTOCOL, "devices": playback_devices()}, ensure_ascii=False), flush=True)
    else:
        def terminate(_signum, _frame):
            raise SystemExit(0)
        signal.signal(signal.SIGTERM, terminate)
        play(sys.stdin.buffer, sys.stdout, args.device, sys.stdout.buffer)


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, RuntimeError) as error:
        print("[Audio] " + str(error), file=sys.stderr, flush=True)
        sys.exit(1)
