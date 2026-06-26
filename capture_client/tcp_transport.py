from __future__ import annotations

import json
import socket
import threading
from collections import defaultdict, deque
from typing import Optional

from .stream_defs import FORMAT_CODE_TO_NAME

from .messages import RawMessage, StreamInfo
from .transport import SensorTransport

HELLO_PREFIX = b"CAPHELLO "
MSG_PREFIX = b"CAPMSG1 "


def _recv_exact(sock: socket.socket, n: int) -> bytes:
    chunks = []
    remaining = n
    while remaining > 0:
        chunk = sock.recv(remaining)
        if not chunk:
            raise EOFError("socket closed")
        chunks.append(chunk)
        remaining -= len(chunk)
    return b"".join(chunks)


def _recv_line(sock: socket.socket, limit: int = 4096) -> bytes:
    data = b""
    while not data.endswith(b"\n"):
        chunk = sock.recv(1)
        if not chunk:
            raise EOFError("socket closed")
        data += chunk
        if len(data) > limit:
            raise ValueError("line too long")
    return data[:-1]


class TcpSensorTransport(SensorTransport):
    def __init__(self, host: str = "127.0.0.1", port: int = 45660, *, required_streams: Optional[list[str]] = None, subscribe_streams: Optional[list[str]] = None, per_stream_buffer: int = 512):
        self.host = host
        self.port = int(port)
        self.required_streams = required_streams or []
        self.subscribe_streams = subscribe_streams or required_streams or []
        self.per_stream_buffer = int(per_stream_buffer)
        self.sock = socket.create_connection((self.host, self.port), timeout=5.0)
        self.sock.settimeout(None)
        self._lock = threading.Lock()
        self._closed = False
        self._latest: dict[str, int] = defaultdict(int)
        self._messages: dict[str, deque[RawMessage]] = defaultdict(lambda: deque(maxlen=self.per_stream_buffer))
        self._streams = self._read_hello()
        missing = [sid for sid in self.required_streams if sid not in self._streams]
        if missing:
            raise KeyError(f"missing required streams in TCP hello: {missing}")
        self._send_subscribe()
        self._thread = threading.Thread(target=self._rx_loop, name="capture_tcp_transport_rx", daemon=True)
        self._thread.start()

    def _read_hello(self) -> dict[str, StreamInfo]:
        line = _recv_line(self.sock)
        parts = line.split()
        if len(parts) != 2 or parts[0] != HELLO_PREFIX.rstrip():
            raise ValueError(f"bad hello line: {line!r}")
        data = json.loads(_recv_exact(self.sock, int(parts[1])).decode("utf-8"))
        streams = {}
        for sid, info in data.get("streams", {}).items():
            streams[sid] = StreamInfo(
                stream_id=sid,
                shm_name="",
                kind=info.get("kind", "UNKNOWN"),
                frame_id=info.get("frame_id", sid),
                width=int(info.get("width", 0)),
                height=int(info.get("height", 0)),
                format_code=int(info.get("format_code", 0)),
                format_name=info.get("format_name", "UNKNOWN"),
                payload_size=int(info.get("payload_size", 0)),
                slot_count=int(info.get("slot_count", 0)),
                description=info.get("description", ""),
                raw=info,
            )
        return streams

    def _send_subscribe(self) -> None:
        line = "SUBSCRIBE " + (",".join(self.subscribe_streams) if self.subscribe_streams else "*") + "\n"
        self.sock.sendall(line.encode("utf-8"))

    def _rx_loop(self) -> None:
        try:
            while not self._closed:
                line = _recv_line(self.sock)
                parts = line.split()
                if len(parts) != 3 or parts[0] != MSG_PREFIX.rstrip():
                    continue
                header = json.loads(_recv_exact(self.sock, int(parts[1])).decode("utf-8"))
                payload = _recv_exact(self.sock, int(parts[2])) if int(parts[2]) else b""
                sid = str(header["stream_id"])
                msg = RawMessage(
                    stream_id=sid,
                    sequence=int(header["sequence"]),
                    timestamp_ns=int(header["timestamp_ns"]),
                    monotonic_ns=int(header.get("monotonic_ns", 0)),
                    payload_size=int(header.get("payload_size", len(payload))),
                    width=int(header.get("width", 0)),
                    height=int(header.get("height", 0)),
                    format_code=int(header.get("format_code", 0)),
                    format_name=header.get("format_name", FORMAT_CODE_TO_NAME.get(int(header.get("format_code", 0)), "UNKNOWN")),
                    flags=int(header.get("flags", 0)),
                    frame_id=header.get("frame_id", sid),
                    payload=payload,
                )
                with self._lock:
                    self._latest[sid] = max(self._latest[sid], msg.sequence)
                    self._messages[sid].append(msg)
        except Exception:
            self._closed = True

    def list_streams(self) -> dict[str, StreamInfo]:
        return dict(self._streams)
    def stream_info(self, stream_id: str) -> StreamInfo:
        return self._streams[stream_id]
    def latest_sequence(self, stream_id: str) -> int:
        with self._lock:
            return int(self._latest.get(stream_id, 0))
    def read_latest(self, stream_id: str, *, copy_payload: bool = True) -> Optional[RawMessage]:
        with self._lock:
            q = self._messages.get(stream_id)
            msg = q[-1] if q else None
        return RawMessage(**{**msg.__dict__, "payload": bytes(msg.payload)}) if (msg and copy_payload) else msg
    def read_sequence(self, stream_id: str, sequence: int, *, copy_payload: bool = True) -> Optional[RawMessage]:
        with self._lock:
            q = list(self._messages.get(stream_id, ()))
        for msg in q:
            if msg.sequence == sequence:
                return RawMessage(**{**msg.__dict__, "payload": bytes(msg.payload)}) if copy_payload else msg
        return None
    def read_range(self, stream_id: str, start_sequence: int, end_sequence: int, *, copy_payload: bool = True) -> list[RawMessage]:
        return [m for m in (self.read_sequence(stream_id, seq, copy_payload=copy_payload) for seq in range(start_sequence, end_sequence + 1)) if m]
    def close(self) -> None:
        self._closed = True
        try: self.sock.shutdown(socket.SHUT_RDWR)
        except Exception: pass
        try: self.sock.close()
        except Exception: pass
