from __future__ import annotations

import struct
from pathlib import Path
from typing import Optional

from .stream_defs import FormatCode

from .messages import ImageFrame, ImuSample, RawMessage, StreamInfo
from .shm_transport import ShmSensorTransport
from .tcp_transport import TcpSensorTransport
from .transport import SensorTransport


class CaptureClient:
    """Transport-neutral reader SDK for capture_service streams.

    This is intended to be embedded into consumers such as calibration recorders,
    hand_tracking_backend, sync-check tools, xr_video backends, and future bridge services.
    """

    def __init__(self, transport: SensorTransport):
        self.transport = transport

    @classmethod
    def from_tcp(cls, host: str = "127.0.0.1", port: int = 45660, *, required_streams: Optional[list[str]] = None, subscribe_streams: Optional[list[str]] = None) -> "CaptureClient":
        return cls(TcpSensorTransport(host, port, required_streams=required_streams, subscribe_streams=subscribe_streams))

    @classmethod
    def from_shm_registry(cls, registry_path: str | Path = "/tmp/capture_service_streams.json", *, required_streams: Optional[list[str]] = None) -> "CaptureClient":
        return cls(ShmSensorTransport(registry_path, required_streams=required_streams))

    def list_streams(self) -> dict[str, StreamInfo]:
        return self.transport.list_streams()

    def latest_sequence(self, stream_id: str) -> int:
        return self.transport.latest_sequence(stream_id)

    def read_latest_raw(self, stream_id: str, *, copy_payload: bool = True) -> Optional[RawMessage]:
        return self.transport.read_latest(stream_id, copy_payload=copy_payload)

    def read_raw_sequence(self, stream_id: str, sequence: int, *, copy_payload: bool = True) -> Optional[RawMessage]:
        return self.transport.read_sequence(stream_id, sequence, copy_payload=copy_payload)

    def read_raw_range(self, stream_id: str, start_sequence: int, end_sequence: int, *, copy_payload: bool = True) -> list[RawMessage]:
        return self.transport.read_range(stream_id, start_sequence, end_sequence, copy_payload=copy_payload)

    def read_latest_image(self, stream_id: str, *, copy_payload: bool = True) -> Optional[ImageFrame]:
        msg = self.read_latest_raw(stream_id, copy_payload=copy_payload)
        return self._image_from_raw(msg) if msg else None

    def read_image_sequence(self, stream_id: str, sequence: int, *, copy_payload: bool = True) -> Optional[ImageFrame]:
        msg = self.read_raw_sequence(stream_id, sequence, copy_payload=copy_payload)
        return self._image_from_raw(msg) if msg else None

    def read_latest_imu(self, stream_id: str = "imu0") -> Optional[ImuSample]:
        msg = self.read_latest_raw(stream_id, copy_payload=True)
        return self._imu_from_raw(msg) if msg else None

    def read_imu_sequence(self, stream_id: str, sequence: int) -> Optional[ImuSample]:
        msg = self.read_raw_sequence(stream_id, sequence, copy_payload=True)
        return self._imu_from_raw(msg) if msg else None

    def read_imu_range(self, stream_id: str, start_sequence: int, end_sequence: int) -> list[ImuSample]:
        samples = []
        for msg in self.read_raw_range(stream_id, start_sequence, end_sequence, copy_payload=True):
            sample = self._imu_from_raw(msg)
            if sample is not None:
                samples.append(sample)
        return samples

    def close(self) -> None:
        self.transport.close()

    def _image_from_raw(self, msg: RawMessage | None) -> Optional[ImageFrame]:
        if msg is None:
            return None
        if msg.format_code not in (int(FormatCode.GRAY8), int(FormatCode.YUY2), int(FormatCode.RGB8), int(FormatCode.BGR8)):
            return None
        return ImageFrame(
            stream_id=msg.stream_id,
            sequence=msg.sequence,
            timestamp_ns=msg.timestamp_ns,
            monotonic_ns=msg.monotonic_ns,
            width=msg.width,
            height=msg.height,
            format_name=msg.format_name,
            frame_id=msg.frame_id,
            data=msg.payload,
        )

    def _imu_from_raw(self, msg: RawMessage | None) -> Optional[ImuSample]:
        if msg is None:
            return None
        if msg.format_code != int(FormatCode.IMU_F32_LE) or msg.payload_size < 24:
            return None
        gx, gy, gz, ax, ay, az = struct.unpack("<ffffff", bytes(msg.payload[:24]))
        return ImuSample(
            stream_id=msg.stream_id,
            sequence=msg.sequence,
            timestamp_ns=msg.timestamp_ns,
            monotonic_ns=msg.monotonic_ns,
            gyro_rad_s=(float(gx), float(gy), float(gz)),
            accel_m_s2=(float(ax), float(ay), float(az)),
            frame_id=msg.frame_id,
        )
