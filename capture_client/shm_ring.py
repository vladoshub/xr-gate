from __future__ import annotations

import json
import os
import struct
import time
from dataclasses import asdict
from multiprocessing import shared_memory, resource_tracker
from pathlib import Path
from typing import Any, Dict, Optional

from .stream_defs import FORMAT_CODE_TO_NAME, StreamKind, StreamSpec

MAGIC = b"CAPSHM1\0"
VERSION = 1
DEFAULT_HEADER_SIZE = 4096
DEFAULT_SLOT_HEADER_SIZE = 128

# magic[8], version, kind, slot_count, slot_header_size, payload_size,
# header_size, created_ns, latest_seq
GLOBAL_HEADER = struct.Struct("<8sIIIIIIQQ")
GLOBAL_HEADER_NO_LATEST = struct.Struct("<8sIIIIIIQ")
# seq_begin, seq_end, timestamp_ns, monotonic_ns, payload_size, width, height,
# format_code, flags, reserved, frame_id[32]
SLOT_HEADER = struct.Struct("<QQqQIIIIII32s")


class ShmRingError(RuntimeError):
    pass


def sanitize_shm_name(name: str) -> str:
    out = []
    for ch in name:
        if ch.isalnum() or ch in ("_", "-"):
            out.append(ch)
        else:
            out.append("_")
    return "cap_" + "".join(out)[:180]


def now_ns() -> int:
    return time.monotonic_ns()


def unregister_from_resource_tracker(shm: shared_memory.SharedMemory) -> None:
    """Do not let Python resource_tracker own producer/attach-only SHM.

    capture_service manages POSIX SHM lifetime manually:
    - stale segments are unlinked on startup;
    - active segments are only closed on shutdown;
    - consumers may still attach by registry/name.

    Python 3.10 resource_tracker otherwise warns about leaked shared_memory
    and may try to unlink segments at interpreter shutdown.
    """
    try:
        resource_tracker.unregister(shm._name, "shared_memory")
    except Exception:
        pass


class ShmRingPublisher:
    """Single-writer, multi-reader POSIX shared-memory ring.

    The producer owns the shared-memory segment and writes fixed-size payload slots.
    Consumers attach by name and read the latest committed slot.

    This is intentionally small and dependency-free; later C++ consumers can implement
    the same struct layout directly.
    """

    def __init__(self, spec: StreamSpec, namespace: str = "capture_service", unlink_existing: bool = True):
        self.spec = spec
        self.namespace = namespace
        self.shm_name = sanitize_shm_name(f"{namespace}_{spec.stream_id}")
        self.header_size = DEFAULT_HEADER_SIZE
        self.slot_header_size = DEFAULT_SLOT_HEADER_SIZE
        self.slot_stride = self.slot_header_size + spec.payload_size
        self.size = self.header_size + spec.slot_count * self.slot_stride
        self.seq = 0
        self._closed = False

        if unlink_existing:
            try:
                old = shared_memory.SharedMemory(name=self.shm_name, create=False)
                old.close()
                old.unlink()
            except FileNotFoundError:
                pass

        self.shm = shared_memory.SharedMemory(name=self.shm_name, create=True, size=self.size)
        unregister_from_resource_tracker(self.shm)
        self.buf = self.shm.buf
        self._write_global_header(latest_seq=0)
        self._write_json_metadata()

    def _write_global_header(self, latest_seq: int) -> None:
        GLOBAL_HEADER.pack_into(
            self.buf,
            0,
            MAGIC,
            VERSION,
            int(self.spec.kind),
            int(self.spec.slot_count),
            int(self.slot_header_size),
            int(self.spec.payload_size),
            int(self.header_size),
            int(time.time_ns()),
            int(latest_seq),
        )

    def _write_latest_seq(self, latest_seq: int) -> None:
        # latest_seq is the last field in GLOBAL_HEADER.
        latest_offset = GLOBAL_HEADER.size - 8
        struct.pack_into("<Q", self.buf, latest_offset, int(latest_seq))

    def _write_json_metadata(self) -> None:
        meta = {
            "stream_id": self.spec.stream_id,
            "kind": StreamKind(self.spec.kind).name,
            "frame_id": self.spec.frame_id,
            "width": self.spec.width,
            "height": self.spec.height,
            "format_code": self.spec.format_code,
            "format_name": FORMAT_CODE_TO_NAME.get(self.spec.format_code, "UNKNOWN"),
            "payload_size": self.spec.payload_size,
            "slot_count": self.spec.slot_count,
            "slot_header_size": self.slot_header_size,
            "slot_stride": self.slot_stride,
            "description": self.spec.description,
            "shm_name": self.shm_name,
            "layout": "GLOBAL_HEADER(<8sIIIIIIQQ) + JSON metadata + slots[SLOT_HEADER(<QQqQIIIIII32s)+payload]",
        }
        raw = json.dumps(meta, ensure_ascii=False, sort_keys=True).encode("utf-8")
        if len(raw) > self.header_size - GLOBAL_HEADER.size - 4:
            raise ShmRingError(f"metadata too large for stream {self.spec.stream_id}")
        offset = GLOBAL_HEADER.size
        struct.pack_into("<I", self.buf, offset, len(raw))
        offset += 4
        self.buf[offset : offset + len(raw)] = raw

    def publish(
        self,
        payload: bytes | memoryview,
        *,
        timestamp_ns: Optional[int] = None,
        width: Optional[int] = None,
        height: Optional[int] = None,
        format_code: Optional[int] = None,
        flags: int = 0,
        frame_id: Optional[str] = None,
    ) -> int:
        if self._closed:
            raise ShmRingError("publisher is closed")
        if timestamp_ns is None:
            timestamp_ns = now_ns()
        mono = now_ns()
        payload_len = len(payload)
        if payload_len > self.spec.payload_size:
            raise ShmRingError(
                f"payload {payload_len} exceeds stream payload_size {self.spec.payload_size} for {self.spec.stream_id}"
            )

        self.seq += 1
        seq = self.seq
        slot_index = (seq - 1) % self.spec.slot_count
        slot_off = self.header_size + slot_index * self.slot_stride
        payload_off = slot_off + self.slot_header_size
        seq_odd = seq * 2 + 1
        seq_even = seq * 2
        fid = (frame_id or self.spec.frame_id or self.spec.stream_id).encode("utf-8")[:31]
        fid = fid + b"\0" * (32 - len(fid))

        # Mark slot as being written.
        SLOT_HEADER.pack_into(
            self.buf,
            slot_off,
            seq_odd,
            0,
            int(timestamp_ns),
            int(mono),
            int(payload_len),
            int(width if width is not None else self.spec.width),
            int(height if height is not None else self.spec.height),
            int(format_code if format_code is not None else self.spec.format_code),
            int(flags),
            0,
            fid,
        )

        self.buf[payload_off : payload_off + payload_len] = payload
        if payload_len < self.spec.payload_size:
            # Avoid stale bytes leaking into short BYTES/IMU messages.
            self.buf[payload_off + payload_len : payload_off + self.spec.payload_size] = b"\0" * (
                self.spec.payload_size - payload_len
            )

        # Commit slot.
        SLOT_HEADER.pack_into(
            self.buf,
            slot_off,
            seq_even,
            seq_even,
            int(timestamp_ns),
            int(mono),
            int(payload_len),
            int(width if width is not None else self.spec.width),
            int(height if height is not None else self.spec.height),
            int(format_code if format_code is not None else self.spec.format_code),
            int(flags),
            0,
            fid,
        )
        self._write_latest_seq(seq)
        return seq

    def close(self, unlink: bool = False) -> None:
        if self._closed:
            return
        self._closed = True
        self.shm.close()
        if unlink:
            try:
                self.shm.unlink()
            except FileNotFoundError:
                pass

    def registry_entry(self) -> Dict[str, Any]:
        return {
            "stream_id": self.spec.stream_id,
            "shm_name": self.shm_name,
            "kind": StreamKind(self.spec.kind).name,
            "frame_id": self.spec.frame_id,
            "width": self.spec.width,
            "height": self.spec.height,
            "format_code": self.spec.format_code,
            "format_name": FORMAT_CODE_TO_NAME.get(self.spec.format_code, "UNKNOWN"),
            "payload_size": self.spec.payload_size,
            "slot_count": self.spec.slot_count,
            "slot_header_size": self.slot_header_size,
            "header_size": self.header_size,
            "slot_stride": self.slot_stride,
            "description": self.spec.description,
        }


class ShmRingConsumer:
    def __init__(self, shm_name: str):
        self.shm_name = shm_name
        self.shm = shared_memory.SharedMemory(name=shm_name, create=False)
        unregister_from_resource_tracker(self.shm)
        self.buf = self.shm.buf
        header = GLOBAL_HEADER.unpack_from(self.buf, 0)
        magic, version, kind, slot_count, slot_header_size, payload_size, header_size, created_ns, latest_seq = header
        if magic != MAGIC:
            raise ShmRingError(f"bad magic for {shm_name}: {magic!r}")
        if version != VERSION:
            raise ShmRingError(f"unsupported shm version {version}")
        self.kind = kind
        self.slot_count = slot_count
        self.slot_header_size = slot_header_size
        self.payload_size = payload_size
        self.header_size = header_size
        self.slot_stride = self.slot_header_size + self.payload_size
        self.last_seq = 0
        self.metadata = self._read_metadata()

    def _read_latest_seq(self) -> int:
        latest_offset = GLOBAL_HEADER.size - 8
        return struct.unpack_from("<Q", self.buf, latest_offset)[0]

    def _read_metadata(self) -> Dict[str, Any]:
        offset = GLOBAL_HEADER.size
        meta_len = struct.unpack_from("<I", self.buf, offset)[0]
        offset += 4
        if meta_len <= 0 or meta_len > self.header_size - offset:
            return {}
        raw = bytes(self.buf[offset : offset + meta_len])
        return json.loads(raw.decode("utf-8"))

    def read_latest(self, copy_payload: bool = True) -> Optional[Dict[str, Any]]:
        latest_seq = self._read_latest_seq()
        if latest_seq == 0:
            return None
        slot_index = (latest_seq - 1) % self.slot_count
        slot_off = self.header_size + slot_index * self.slot_stride
        payload_off = slot_off + self.slot_header_size

        h1 = SLOT_HEADER.unpack_from(self.buf, slot_off)
        seq_begin, seq_end, timestamp_ns, mono, payload_len, width, height, format_code, flags, reserved, frame_id_raw = h1
        if seq_begin != seq_end or seq_begin % 2 != 0 or seq_begin == 0:
            return None
        payload_view = self.buf[payload_off : payload_off + payload_len]
        payload = bytes(payload_view) if copy_payload else payload_view
        h2 = SLOT_HEADER.unpack_from(self.buf, slot_off)
        if h1 != h2:
            return None
        sequence = seq_end // 2
        frame_id = frame_id_raw.split(b"\0", 1)[0].decode("utf-8", errors="replace")
        return {
            "sequence": sequence,
            "timestamp_ns": timestamp_ns,
            "monotonic_ns": mono,
            "payload_size": payload_len,
            "width": width,
            "height": height,
            "format_code": format_code,
            "format_name": FORMAT_CODE_TO_NAME.get(format_code, "UNKNOWN"),
            "flags": flags,
            "frame_id": frame_id,
            "payload": payload,
        }

    def close(self) -> None:
        self.shm.close()


class StreamRegistry:
    def __init__(self, path: str | Path):
        self.path = Path(path)

    def write(
        self,
        *,
        namespace: str,
        publishers: Dict[str, ShmRingPublisher] | None = None,
        entries: Dict[str, Dict[str, Any]] | None = None,
        config_path: str,
        extra: Dict[str, Any] | None = None,
    ) -> None:
        streams: Dict[str, Any] = {}
        if publishers:
            streams.update({sid: pub.registry_entry() for sid, pub in publishers.items()})
        if entries:
            streams.update(entries)
        data = {
            "service": "capture_service",
            "namespace": namespace,
            "pid": os.getpid(),
            "created_unix_ns": time.time_ns(),
            "config_path": str(config_path),
            "streams": streams,
        }
        if extra:
            data.update(extra)
        self.path.parent.mkdir(parents=True, exist_ok=True)
        tmp = self.path.with_suffix(self.path.suffix + ".tmp")
        tmp.write_text(json.dumps(data, indent=2, ensure_ascii=False, sort_keys=True), encoding="utf-8")
        tmp.replace(self.path)
