from __future__ import annotations

import json
import struct
from pathlib import Path
from typing import Any, Optional

from .shm_ring import ShmRingConsumer, SLOT_HEADER
from .stream_defs import FORMAT_CODE_TO_NAME

from .messages import RawMessage, StreamInfo
from .transport import SensorTransport


class ShmStreamReader:
    def __init__(self, stream_id: str, registry_info: dict[str, Any]):
        self.stream_id = stream_id
        self.registry_info = registry_info
        self.consumer = ShmRingConsumer(registry_info["shm_name"])
        self.info = StreamInfo(
            stream_id=stream_id,
            shm_name=registry_info["shm_name"],
            kind=registry_info.get("kind", "UNKNOWN"),
            frame_id=registry_info.get("frame_id", stream_id),
            width=int(registry_info.get("width", 0)),
            height=int(registry_info.get("height", 0)),
            format_code=int(registry_info.get("format_code", 0)),
            format_name=registry_info.get("format_name", "UNKNOWN"),
            payload_size=int(registry_info.get("payload_size", 0)),
            slot_count=int(registry_info.get("slot_count", self.consumer.slot_count)),
            description=registry_info.get("description", ""),
            raw=registry_info,
        )

    def latest_sequence(self) -> int:
        return self.consumer._read_latest_seq()

    def _convert_msg(self, d: dict[str, Any]) -> RawMessage:
        return RawMessage(
            stream_id=self.stream_id,
            sequence=int(d["sequence"]),
            timestamp_ns=int(d["timestamp_ns"]),
            monotonic_ns=int(d["monotonic_ns"]),
            payload_size=int(d["payload_size"]),
            width=int(d["width"]),
            height=int(d["height"]),
            format_code=int(d["format_code"]),
            format_name=d.get("format_name", FORMAT_CODE_TO_NAME.get(int(d["format_code"]), "UNKNOWN")),
            flags=int(d.get("flags", 0)),
            frame_id=d.get("frame_id", self.stream_id),
            payload=d["payload"],
        )

    def read_latest(self, *, copy_payload: bool = True) -> Optional[RawMessage]:
        d = self.consumer.read_latest(copy_payload=copy_payload)
        return self._convert_msg(d) if d else None

    def read_sequence(self, sequence: int, *, copy_payload: bool = True) -> Optional[RawMessage]:
        if sequence <= 0:
            return None
        latest_seq = self.latest_sequence()
        if latest_seq == 0:
            return None
        if sequence > latest_seq:
            return None
        if latest_seq - sequence >= self.consumer.slot_count:
            # Requested slot has already been overwritten.
            return None

        slot_index = (sequence - 1) % self.consumer.slot_count
        slot_off = self.consumer.header_size + slot_index * self.consumer.slot_stride
        payload_off = slot_off + self.consumer.slot_header_size

        h1 = SLOT_HEADER.unpack_from(self.consumer.buf, slot_off)
        seq_begin, seq_end, timestamp_ns, mono, payload_len, width, height, format_code, flags, _reserved, frame_id_raw = h1
        if seq_begin != seq_end or seq_begin % 2 != 0 or seq_begin == 0:
            return None
        actual_sequence = seq_end // 2
        if actual_sequence != sequence:
            return None

        payload_view = self.consumer.buf[payload_off : payload_off + payload_len]
        payload = bytes(payload_view) if copy_payload else payload_view

        h2 = SLOT_HEADER.unpack_from(self.consumer.buf, slot_off)
        if h1 != h2:
            return None

        frame_id = frame_id_raw.split(b"\0", 1)[0].decode("utf-8", errors="replace")
        return RawMessage(
            stream_id=self.stream_id,
            sequence=actual_sequence,
            timestamp_ns=int(timestamp_ns),
            monotonic_ns=int(mono),
            payload_size=int(payload_len),
            width=int(width),
            height=int(height),
            format_code=int(format_code),
            format_name=FORMAT_CODE_TO_NAME.get(int(format_code), "UNKNOWN"),
            flags=int(flags),
            frame_id=frame_id,
            payload=payload,
        )

    def read_range(self, start_sequence: int, end_sequence: int, *, copy_payload: bool = True) -> list[RawMessage]:
        if end_sequence < start_sequence:
            return []
        out: list[RawMessage] = []
        for seq in range(start_sequence, end_sequence + 1):
            msg = self.read_sequence(seq, copy_payload=copy_payload)
            if msg is not None:
                out.append(msg)
        return out

    def close(self) -> None:
        self.consumer.close()


class ShmSensorTransport(SensorTransport):
    def __init__(self, registry_path: str | Path = "/tmp/capture_service_streams.json", *, required_streams: Optional[list[str]] = None):
        self.registry_path = Path(registry_path)
        if not self.registry_path.exists():
            raise FileNotFoundError(f"capture_service registry not found: {self.registry_path}")
        self.registry = json.loads(self.registry_path.read_text(encoding="utf-8"))
        streams = self.registry.get("streams", {})
        if required_streams:
            missing = [sid for sid in required_streams if sid not in streams]
            if missing:
                raise KeyError(f"missing required streams in registry: {missing}")
        self._readers: dict[str, ShmStreamReader] = {
            sid: ShmStreamReader(sid, info) for sid, info in streams.items()
        }
        self.rig = self.registry.get("rig", {})

    def list_streams(self) -> dict[str, StreamInfo]:
        return {sid: reader.info for sid, reader in self._readers.items()}

    def stream_info(self, stream_id: str) -> StreamInfo:
        return self._readers[stream_id].info

    def latest_sequence(self, stream_id: str) -> int:
        return self._readers[stream_id].latest_sequence()

    def read_latest(self, stream_id: str, *, copy_payload: bool = True) -> Optional[RawMessage]:
        return self._readers[stream_id].read_latest(copy_payload=copy_payload)

    def read_sequence(self, stream_id: str, sequence: int, *, copy_payload: bool = True) -> Optional[RawMessage]:
        return self._readers[stream_id].read_sequence(sequence, copy_payload=copy_payload)

    def read_range(self, stream_id: str, start_sequence: int, end_sequence: int, *, copy_payload: bool = True) -> list[RawMessage]:
        return self._readers[stream_id].read_range(start_sequence, end_sequence, copy_payload=copy_payload)

    def close(self) -> None:
        for reader in self._readers.values():
            try:
                reader.close()
            except Exception:
                pass
