from __future__ import annotations

import time
from dataclasses import dataclass
from typing import Optional

from .client import CaptureClient
from .messages import ImageFrame, ImuSample, StereoPair, SyncedStereoImu


class StereoPairReader:
    def __init__(self, client: CaptureClient, cam0_stream: str = "camera0", cam1_stream: str = "camera1", *, max_timestamp_delta_ns: int = 1_000_000):
        self.client = client
        self.cam0_stream = cam0_stream
        self.cam1_stream = cam1_stream
        self.max_timestamp_delta_ns = max_timestamp_delta_ns
        self.last_pair_seq = min(client.latest_sequence(cam0_stream), client.latest_sequence(cam1_stream))

    def read_next_pair(self, *, timeout_s: float = 1.0, copy_payload: bool = True) -> Optional[StereoPair]:
        deadline = time.monotonic() + timeout_s
        while time.monotonic() < deadline:
            latest0 = self.client.latest_sequence(self.cam0_stream)
            latest1 = self.client.latest_sequence(self.cam1_stream)
            target = min(latest0, latest1)
            if target <= self.last_pair_seq:
                time.sleep(0.001)
                continue

            # Try sequentially from last+1 to target; if old slots were overwritten,
            # jump to the latest common sequence.
            for seq in range(self.last_pair_seq + 1, target + 1):
                cam0 = self.client.read_image_sequence(self.cam0_stream, seq, copy_payload=copy_payload)
                cam1 = self.client.read_image_sequence(self.cam1_stream, seq, copy_payload=copy_payload)
                if cam0 is None or cam1 is None:
                    continue
                delta = abs(cam0.timestamp_ns - cam1.timestamp_ns)
                self.last_pair_seq = seq
                if delta <= self.max_timestamp_delta_ns:
                    ts = max(cam0.timestamp_ns, cam1.timestamp_ns)
                    return StereoPair(timestamp_ns=ts, cam0=cam0, cam1=cam1)

            self.last_pair_seq = target
        return None


class ImuWindowReader:
    def __init__(self, client: CaptureClient, imu_stream: str = "imu0"):
        self.client = client
        self.imu_stream = imu_stream
        self.last_imu_seq = client.latest_sequence(imu_stream)

    def wait_until_at_least(self, timestamp_ns: int, *, timeout_s: float = 0.05) -> None:
        deadline = time.monotonic() + timeout_s
        while time.monotonic() < deadline:
            latest = self.client.read_latest_imu(self.imu_stream)
            if latest is not None and latest.timestamp_ns >= timestamp_ns:
                return
            time.sleep(0.0005)

    def read_window_until(self, timestamp_ns: int, *, include_future_tolerance_ns: int = 0) -> tuple[ImuSample, ...]:
        latest_seq = self.client.latest_sequence(self.imu_stream)
        if latest_seq <= self.last_imu_seq:
            return tuple()

        samples = []
        new_last = self.last_imu_seq
        for seq in range(self.last_imu_seq + 1, latest_seq + 1):
            sample = self.client.read_imu_sequence(self.imu_stream, seq)
            if sample is None:
                continue
            if sample.timestamp_ns <= timestamp_ns + include_future_tolerance_ns:
                samples.append(sample)
                new_last = seq
            else:
                break

        self.last_imu_seq = max(self.last_imu_seq, new_last)
        return tuple(samples)


class BasaltStereoImuSynchronizer:
    """Builds stereo frame + IMU window packets for future Basalt backend.

    This does not call Basalt. It only performs transport-neutral synchronization
    suitable for a VIO consumer:
      - next stereo pair
      - all IMU samples after previous frame and up to current frame timestamp
    """

    def __init__(
        self,
        client: CaptureClient,
        *,
        cam0_stream: str = "camera0",
        cam1_stream: str = "camera1",
        imu_stream: str = "imu0",
        stereo_max_delta_ns: int = 1_000_000,
        wait_for_imu_s: float = 0.05,
    ):
        self.client = client
        self.pairs = StereoPairReader(client, cam0_stream, cam1_stream, max_timestamp_delta_ns=stereo_max_delta_ns)
        self.imu = ImuWindowReader(client, imu_stream)
        self.wait_for_imu_s = wait_for_imu_s
        self.previous_camera_timestamp_ns: Optional[int] = None

    def read_next(self, *, timeout_s: float = 1.0, copy_images: bool = True) -> Optional[SyncedStereoImu]:
        pair = self.pairs.read_next_pair(timeout_s=timeout_s, copy_payload=copy_images)
        if pair is None:
            return None
        self.imu.wait_until_at_least(pair.timestamp_ns, timeout_s=self.wait_for_imu_s)
        samples = self.imu.read_window_until(pair.timestamp_ns)
        out = SyncedStereoImu(
            pair=pair,
            imu_samples=samples,
            previous_camera_timestamp_ns=self.previous_camera_timestamp_ns,
        )
        self.previous_camera_timestamp_ns = pair.timestamp_ns
        return out
