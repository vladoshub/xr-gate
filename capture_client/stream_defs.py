from __future__ import annotations

from dataclasses import dataclass
from enum import IntEnum
from typing import Dict


class StreamKind(IntEnum):
    IMAGE = 1
    IMU = 2
    BYTES = 3


class FormatCode(IntEnum):
    UNKNOWN = 0
    GRAY8 = 1
    YUY2 = 2
    RGB8 = 3
    BGR8 = 4
    IMU_F32_LE = 101      # payload: struct '<ffffff' = gyro xyz rad/s, accel xyz m/s^2
    BYTES = 255


FORMAT_NAME_TO_CODE: Dict[str, int] = {
    "UNKNOWN": int(FormatCode.UNKNOWN),
    "GRAY8": int(FormatCode.GRAY8),
    "YUY2": int(FormatCode.YUY2),
    "RGB8": int(FormatCode.RGB8),
    "BGR8": int(FormatCode.BGR8),
    "IMU_F32_LE": int(FormatCode.IMU_F32_LE),
    "BYTES": int(FormatCode.BYTES),
}

FORMAT_CODE_TO_NAME: Dict[int, str] = {v: k for k, v in FORMAT_NAME_TO_CODE.items()}


@dataclass(frozen=True)
class StreamSpec:
    stream_id: str
    kind: StreamKind
    width: int = 0
    height: int = 0
    format_code: int = int(FormatCode.UNKNOWN)
    payload_size: int = 0
    slot_count: int = 8
    frame_id: str = ""
    description: str = ""


@dataclass(frozen=True)
class CameraFrameMeta:
    stream_id: str
    frame_id: str
    timestamp_ns: int
    monotonic_ns: int
    sequence: int
    width: int
    height: int
    format_code: int
    payload_size: int


@dataclass(frozen=True)
class ImuSample:
    timestamp_ns: int
    frame_id: str
    gyro_rad_s: tuple[float, float, float]
    accel_m_s2: tuple[float, float, float]
