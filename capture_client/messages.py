from __future__ import annotations

from dataclasses import dataclass
from typing import Dict, Mapping, Optional, Tuple


@dataclass(frozen=True)
class StreamInfo:
    stream_id: str
    shm_name: str
    kind: str
    frame_id: str
    width: int
    height: int
    format_code: int
    format_name: str
    payload_size: int
    slot_count: int
    description: str = ""
    raw: Optional[Mapping] = None


@dataclass(frozen=True)
class RawMessage:
    stream_id: str
    sequence: int
    timestamp_ns: int
    monotonic_ns: int
    payload_size: int
    width: int
    height: int
    format_code: int
    format_name: str
    flags: int
    frame_id: str
    payload: bytes | memoryview


@dataclass(frozen=True)
class ImageFrame:
    stream_id: str
    sequence: int
    timestamp_ns: int
    monotonic_ns: int
    width: int
    height: int
    format_name: str
    frame_id: str
    data: bytes | memoryview


@dataclass(frozen=True)
class ImuSample:
    stream_id: str
    sequence: int
    timestamp_ns: int
    monotonic_ns: int
    gyro_rad_s: Tuple[float, float, float]
    accel_m_s2: Tuple[float, float, float]
    frame_id: str = "imu0"


@dataclass(frozen=True)
class StereoPair:
    timestamp_ns: int
    cam0: ImageFrame
    cam1: ImageFrame

    @property
    def sequence(self) -> int:
        return min(self.cam0.sequence, self.cam1.sequence)

    @property
    def timestamp_delta_ns(self) -> int:
        return self.cam0.timestamp_ns - self.cam1.timestamp_ns


@dataclass(frozen=True)
class SyncedStereoImu:
    pair: StereoPair
    imu_samples: Tuple[ImuSample, ...]
    previous_camera_timestamp_ns: Optional[int]

    @property
    def camera_timestamp_ns(self) -> int:
        return self.pair.timestamp_ns

    @property
    def imu_count(self) -> int:
        return len(self.imu_samples)

    @property
    def latest_imu_delta_ms(self) -> Optional[float]:
        if not self.imu_samples:
            return None
        return (self.imu_samples[-1].timestamp_ns - self.camera_timestamp_ns) / 1e6

    @property
    def oldest_imu_delta_ms(self) -> Optional[float]:
        if not self.imu_samples:
            return None
        return (self.imu_samples[0].timestamp_ns - self.camera_timestamp_ns) / 1e6
