from __future__ import annotations

from abc import ABC, abstractmethod
from typing import Iterable, Optional

from .messages import RawMessage, StreamInfo


class SensorTransport(ABC):
    """Transport-neutral stream reader interface.

    First implementation is SHM. Later implementations can be TCP/UDP/Unix socket,
    file replay, WSL bridge, etc. Consumers should depend on this interface, not on
    capture_service internals.
    """

    @abstractmethod
    def list_streams(self) -> dict[str, StreamInfo]:
        raise NotImplementedError

    @abstractmethod
    def stream_info(self, stream_id: str) -> StreamInfo:
        raise NotImplementedError

    @abstractmethod
    def latest_sequence(self, stream_id: str) -> int:
        raise NotImplementedError

    @abstractmethod
    def read_latest(self, stream_id: str, *, copy_payload: bool = True) -> Optional[RawMessage]:
        raise NotImplementedError

    @abstractmethod
    def read_sequence(self, stream_id: str, sequence: int, *, copy_payload: bool = True) -> Optional[RawMessage]:
        raise NotImplementedError

    @abstractmethod
    def read_range(self, stream_id: str, start_sequence: int, end_sequence: int, *, copy_payload: bool = True) -> list[RawMessage]:
        raise NotImplementedError

    @abstractmethod
    def close(self) -> None:
        raise NotImplementedError
