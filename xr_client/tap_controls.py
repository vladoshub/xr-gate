#!/usr/bin/env python3
"""IMU tap detection for xr_backend_client.

This module intentionally contains only tap source/detection/config parsing.
Lifecycle actions stay in xr_backend_client because they own child processes,
registries and service specs.
"""
from __future__ import annotations

import json
import math
import os
import queue
import sys
import threading
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Callable, Dict, Iterable, List, Optional, Sequence, Tuple


@dataclass
class ImuTapSourceSpec:
    transport: str = "shm"
    registry: str = "/tmp/capture_service_streams.json"
    imu_stream: str = "imu0"
    tcp_host: str = "127.0.0.1"
    tcp_port: int = 45660
    poll_sleep_s: float = 0.001


@dataclass
class ImuTapDetectorSpec:
    tap_accel_threshold: float = 15.0
    tap_refractory_ms: float = 100.0
    impact_end_ratio: float = 0.70
    impact_max_width_ms: float = 60.0

    double_min_interval_ms: float = 250.0
    double_max_interval_ms: float = 650.0
    double_max_span_ms: float = 900.0
    double_emit_delay_ms: float = 700.0

    triple_min_interval_ms: float = 250.0
    triple_max_interval_ms: float = 650.0
    triple_max_span_ms: float = 1400.0
    triple_emit_delay_ms: float = 750.0

    quadruple_min_interval_ms: float = 250.0
    quadruple_max_interval_ms: float = 650.0
    quadruple_max_span_ms: float = 2100.0

    cooldown_ms: float = 4000.0
    side_axis: str = "ax"
    side_deadzone_mps2: float = 3.0


@dataclass
class ImuTapControlsSpec:
    enabled: bool = False
    debug: bool = True
    source: ImuTapSourceSpec = field(default_factory=ImuTapSourceSpec)
    detector: ImuTapDetectorSpec = field(default_factory=ImuTapDetectorSpec)
    actions: Dict[str, Dict[str, Any]] = field(default_factory=dict)


@dataclass(frozen=True)
class TapCandidate:
    timestamp_ns: int
    accel_peak: float
    gyro_peak: float
    ax: float
    ay: float
    az: float
    gx: float
    gy: float
    gz: float
    side: str


@dataclass(frozen=True)
class TapEvent:
    name: str
    side: str
    candidates: Tuple[TapCandidate, ...] = field(default_factory=tuple)
    confidence: float = 1.0
    timestamp_ns: int = 0


def expand_path(value: str, root_project: Optional[str] = None) -> str:
    value = os.path.expanduser(os.path.expandvars(str(value)))
    if root_project:
        value = value.replace("{root}", root_project)
    value = value.replace("{python}", sys.executable)
    return value


def env_truthy(name: str, default: bool) -> bool:
    raw = os.environ.get(name)
    if raw is None:
        return default
    return raw.strip().lower() not in ("0", "false", "no", "off", "")


def parse_imu_tap_controls(item: Optional[Dict[str, Any]], root_project: str) -> Optional[ImuTapControlsSpec]:
    if not item:
        return None
    enabled = env_truthy(str(item.get("enable_env", "RUN_IMU_TAP_CONTROLS")), bool(item.get("enabled", False)))
    source_raw = dict(item.get("source", {}))
    detector_raw = dict(item.get("detector", {}))
    source = ImuTapSourceSpec(
        transport=str(source_raw.get("transport", "shm")),
        registry=expand_path(str(source_raw.get("registry", "/tmp/capture_service_streams.json")), root_project),
        imu_stream=str(source_raw.get("imu_stream", "imu0")),
        tcp_host=str(source_raw.get("tcp_host", "127.0.0.1")),
        tcp_port=int(source_raw.get("tcp_port", 45660)),
        poll_sleep_s=max(0.0005, float(source_raw.get("poll_sleep_ms", 1.0)) / 1000.0),
    )
    detector = ImuTapDetectorSpec(
        tap_accel_threshold=float(detector_raw.get("tap_accel_threshold", 15.0)),
        tap_refractory_ms=float(detector_raw.get("tap_refractory_ms", 100.0)),
        impact_end_ratio=float(detector_raw.get("impact_end_ratio", 0.70)),
        impact_max_width_ms=float(detector_raw.get("impact_max_width_ms", 60.0)),
        double_min_interval_ms=float(detector_raw.get("double_min_interval_ms", detector_raw.get("triple_min_interval_ms", 250.0))),
        double_max_interval_ms=float(detector_raw.get("double_max_interval_ms", detector_raw.get("triple_max_interval_ms", 650.0))),
        double_max_span_ms=float(detector_raw.get("double_max_span_ms", 900.0)),
        double_emit_delay_ms=float(detector_raw.get("double_emit_delay_ms", 700.0)),
        triple_min_interval_ms=float(detector_raw.get("triple_min_interval_ms", 250.0)),
        triple_max_interval_ms=float(detector_raw.get("triple_max_interval_ms", 650.0)),
        triple_max_span_ms=float(detector_raw.get("triple_max_span_ms", 1400.0)),
        triple_emit_delay_ms=float(detector_raw.get("triple_emit_delay_ms", 750.0)),
        quadruple_min_interval_ms=float(detector_raw.get("quadruple_min_interval_ms", detector_raw.get("triple_min_interval_ms", 250.0))),
        quadruple_max_interval_ms=float(detector_raw.get("quadruple_max_interval_ms", detector_raw.get("triple_max_interval_ms", 650.0))),
        quadruple_max_span_ms=float(detector_raw.get("quadruple_max_span_ms", 2100.0)),
        cooldown_ms=float(detector_raw.get("cooldown_ms", 4000.0)),
        side_axis=str(detector_raw.get("side_axis", "ax")),
        side_deadzone_mps2=float(detector_raw.get("side_deadzone_mps2", 3.0)),
    )
    return ImuTapControlsSpec(
        enabled=enabled,
        debug=bool(item.get("debug", True)),
        source=source,
        detector=detector,
        actions={str(k): dict(v) for k, v in dict(item.get("actions", {})).items()},
    )


def _prepend_sys_path(path: Path) -> None:
    try:
        resolved = path.resolve()
    except Exception:
        resolved = path
    text = str(resolved)
    if path.exists() and text not in sys.path:
        sys.path.insert(0, text)


def _install_capture_service_imports(root_project: str) -> None:
    root = Path(root_project).expanduser()
    candidates = [
        root / "bin" / "python",
        root / "bin" / "python" / "capture_service",
        root / "capture_service",
        root,
    ]
    for env_name in ("XR_PACKAGE_ROOT", "XR_ROOT_PROJECT", "ROOT_PROJECT"):
        value = os.environ.get(env_name)
        if not value:
            continue
        env_root = Path(value).expanduser()
        candidates.extend([
            env_root / "bin" / "python",
            env_root / "bin" / "python" / "capture_service",
            env_root / "capture_service",
            env_root,
        ])
    for candidate in candidates:
        if (candidate / "capture_client").exists():
            _prepend_sys_path(candidate)


def _load_capture_client(root_project: str):
    _install_capture_service_imports(root_project)
    try:
        from capture_client import CaptureClient  # type: ignore
    except Exception as exc:
        raise RuntimeError(
            "failed to import capture_client for IMU tap controls; run from xr_tracking root, "
            "set ROOT_PROJECT, or add bin/python / project root to PYTHONPATH"
        ) from exc
    return CaptureClient


class ImuTapDetector:
    def __init__(
        self,
        cfg: ImuTapControlsSpec,
        root_project: str,
        event_queue: "queue.Queue[TapEvent]",
        logger: Optional[Callable[[str], None]] = None,
    ) -> None:
        self.cfg = cfg
        self.root_project = root_project
        self.event_queue = event_queue
        self.log = logger or (lambda msg: print(msg, flush=True))
        self.stop_event = threading.Event()
        self.thread: Optional[threading.Thread] = None
        self.client: Any = None
        self.last_seq = 0
        self.last_tap_ns = 0
        self.last_event_ns = 0
        self.active_peak: Optional[Dict[str, Any]] = None
        self.candidates: List[TapCandidate] = []
        self.pending_event: Optional[TapEvent] = None
        self.pending_deadline_ns = 0

    def start(self) -> None:
        if self.thread is not None:
            return
        self.thread = threading.Thread(target=self._run, name="imu-tap-detector", daemon=True)
        self.thread.start()

    def stop(self) -> None:
        self.stop_event.set()
        if self.thread is not None:
            self.thread.join(timeout=1.0)
        if self.client is not None:
            try:
                self.client.close()
            except Exception:
                pass
            self.client = None

    def _connect(self) -> None:
        CaptureClient = _load_capture_client(self.root_project)
        source = self.cfg.source
        if source.transport == "shm":
            self.client = CaptureClient.from_shm_registry(source.registry, required_streams=[source.imu_stream])
        elif source.transport == "tcp":
            self.client = CaptureClient.from_tcp(
                source.tcp_host,
                source.tcp_port,
                required_streams=[source.imu_stream],
                subscribe_streams=[source.imu_stream],
            )
        else:
            raise RuntimeError(f"unsupported IMU tap transport: {source.transport}")
        streams = self.client.list_streams()
        if source.imu_stream not in streams:
            raise RuntimeError(f"IMU stream {source.imu_stream!r} not found; streams={list(streams.keys())}")
        self.last_seq = int(self.client.latest_sequence(source.imu_stream))
        self.log(
            "IMU tap controls attached: "
            f"transport={source.transport} stream={source.imu_stream} latest_seq={self.last_seq}"
        )

    def _read_new_samples(self) -> List[Any]:
        assert self.client is not None
        source = self.cfg.source
        latest = int(self.client.latest_sequence(source.imu_stream))
        if latest <= self.last_seq:
            return []
        samples = self.client.read_imu_range(source.imu_stream, self.last_seq + 1, latest)
        self.last_seq = latest
        return samples

    def _side_from_peak(self, peak: Dict[str, Any]) -> str:
        axis = self.cfg.detector.side_axis.lower()
        value = float(peak.get(axis, 0.0))
        if abs(value) < self.cfg.detector.side_deadzone_mps2:
            return "unknown"
        # Dataset 20260618 showed left temple taps have negative ax and right temple taps positive ax.
        if axis == "ax":
            return "left" if value < 0.0 else "right"
        return "negative" if value < 0.0 else "positive"

    def _candidate_from_peak(self, peak: Dict[str, Any]) -> TapCandidate:
        side = self._side_from_peak(peak)
        return TapCandidate(
            timestamp_ns=int(peak["timestamp_ns"]),
            accel_peak=float(peak["accel_peak"]),
            gyro_peak=float(peak["gyro_peak"]),
            ax=float(peak["ax"]),
            ay=float(peak["ay"]),
            az=float(peak["az"]),
            gx=float(peak["gx"]),
            gy=float(peak["gy"]),
            gz=float(peak["gz"]),
            side=side,
        )

    def _begin_peak(self, sample: Any, accel_norm: float, gyro_norm: float) -> None:
        gx, gy, gz = sample.gyro_rad_s
        ax, ay, az = sample.accel_m_s2
        ts = int(sample.monotonic_ns)
        self.active_peak = {
            "start_ns": ts,
            "timestamp_ns": ts,
            "accel_peak": accel_norm,
            "gyro_peak": gyro_norm,
            "ax": float(ax),
            "ay": float(ay),
            "az": float(az),
            "gx": float(gx),
            "gy": float(gy),
            "gz": float(gz),
        }

    def _update_peak(self, sample: Any, accel_norm: float, gyro_norm: float) -> None:
        assert self.active_peak is not None
        if accel_norm <= float(self.active_peak["accel_peak"]):
            return
        gx, gy, gz = sample.gyro_rad_s
        ax, ay, az = sample.accel_m_s2
        self.active_peak.update(
            {
                "timestamp_ns": int(sample.monotonic_ns),
                "accel_peak": accel_norm,
                "gyro_peak": gyro_norm,
                "ax": float(ax),
                "ay": float(ay),
                "az": float(az),
                "gx": float(gx),
                "gy": float(gy),
                "gz": float(gz),
            }
        )

    def _finish_peak(self) -> None:
        if self.active_peak is None:
            return
        cand = self._candidate_from_peak(self.active_peak)
        self.active_peak = None
        self.last_tap_ns = cand.timestamp_ns
        self._add_candidate(cand)

    def _side_for_taps(self, taps: Sequence[TapCandidate]) -> Tuple[str, float]:
        counts = {"left": 0, "right": 0, "unknown": 0}
        for tap in taps:
            counts[tap.side if tap.side in counts else "unknown"] += 1
        side, count = max(counts.items(), key=lambda kv: kv[1])
        confidence = count / float(max(1, len(taps)))
        if side == "unknown" or confidence < 0.5:
            return "unknown", confidence
        return side, confidence

    @staticmethod
    def _valid_tap_sequence(taps: Sequence[TapCandidate], min_interval_ms: float, max_interval_ms: float, max_span_ms: float) -> bool:
        if len(taps) < 2:
            return False
        for left, right in zip(taps[:-1], taps[1:]):
            dt_ms = (right.timestamp_ns - left.timestamp_ns) / 1e6
            if dt_ms < min_interval_ms or dt_ms > max_interval_ms:
                return False
        span_ms = (taps[-1].timestamp_ns - taps[0].timestamp_ns) / 1e6
        return span_ms <= max_span_ms

    def _make_event(self, taps: Sequence[TapCandidate], gesture: str) -> TapEvent:
        side, confidence = self._side_for_taps(taps)
        event_name = f"{side}-{gesture}-tap" if side in ("left", "right") else f"unknown-{gesture}-tap"
        return TapEvent(
            name=event_name,
            side=side,
            candidates=tuple(taps),
            confidence=confidence,
            timestamp_ns=int(taps[-1].timestamp_ns),
        )

    def _emit_event(self, event: TapEvent) -> None:
        d = self.cfg.detector
        now_ns = event.timestamp_ns
        if self.last_event_ns and now_ns - self.last_event_ns < int(d.cooldown_ms * 1_000_000.0):
            if self.cfg.debug:
                self.log(f"[imu_tap] {event.name} ignored: cooldown active")
            self.candidates.clear()
            self.pending_event = None
            self.pending_deadline_ns = 0
            return
        self.last_event_ns = now_ns
        self.candidates.clear()
        self.pending_event = None
        self.pending_deadline_ns = 0
        if self.cfg.debug:
            self.log(f"[imu_tap] event={event.name} confidence={event.confidence:.2f} taps={len(event.candidates)}")
        self.event_queue.put(event)

    def _set_pending(self, event: TapEvent, delay_ms: float) -> None:
        self.pending_event = event
        self.pending_deadline_ns = int(event.timestamp_ns + delay_ms * 1_000_000.0)
        if self.cfg.debug:
            self.log(f"[imu_tap] pending event={event.name} delay={delay_ms:.0f}ms")

    def _flush_pending(self, now_ns: int) -> None:
        if self.pending_event is None:
            return
        if now_ns < self.pending_deadline_ns:
            return
        self._emit_event(self.pending_event)

    def _add_candidate(self, cand: TapCandidate) -> None:
        d = self.cfg.detector
        now_ns = cand.timestamp_ns
        max_keep_ms = max(d.quadruple_max_span_ms, d.triple_max_span_ms, d.double_max_span_ms)
        max_keep_ns = int(max_keep_ms * 1_000_000.0)
        self.candidates = [x for x in self.candidates if now_ns - x.timestamp_ns <= max_keep_ns]
        self.candidates.append(cand)
        self.pending_event = None
        self.pending_deadline_ns = 0
        if self.cfg.debug:
            self.log(
                "[imu_tap] candidate "
                f"side={cand.side} accel={cand.accel_peak:.3f} gyro={cand.gyro_peak:.3f} "
                f"ax={cand.ax:.3f} ay={cand.ay:.3f} az={cand.az:.3f}"
            )

        if len(self.candidates) >= 4:
            taps = self.candidates[-4:]
            if self._valid_tap_sequence(taps, d.quadruple_min_interval_ms, d.quadruple_max_interval_ms, d.quadruple_max_span_ms):
                self._emit_event(self._make_event(taps, "quadruple"))
                return

        if len(self.candidates) >= 3:
            taps = self.candidates[-3:]
            if self._valid_tap_sequence(taps, d.triple_min_interval_ms, d.triple_max_interval_ms, d.triple_max_span_ms):
                self._set_pending(self._make_event(taps, "triple"), d.triple_emit_delay_ms)
                return

        if len(self.candidates) >= 2:
            taps = self.candidates[-2:]
            if self._valid_tap_sequence(taps, d.double_min_interval_ms, d.double_max_interval_ms, d.double_max_span_ms):
                self._set_pending(self._make_event(taps, "double"), d.double_emit_delay_ms)

    def _process_sample(self, sample: Any) -> None:
        gx, gy, gz = sample.gyro_rad_s
        ax, ay, az = sample.accel_m_s2
        accel_norm = math.sqrt(ax * ax + ay * ay + az * az)
        gyro_norm = math.sqrt(gx * gx + gy * gy + gz * gz)
        ts = int(sample.monotonic_ns)
        d = self.cfg.detector
        threshold = d.tap_accel_threshold
        if self.active_peak is not None:
            self._update_peak(sample, accel_norm, gyro_norm)
            age_ms = (ts - int(self.active_peak["start_ns"])) / 1e6
            if accel_norm < threshold * d.impact_end_ratio or age_ms >= d.impact_max_width_ms:
                self._finish_peak()
            return
        if accel_norm < threshold:
            return
        if self.last_tap_ns and ts - self.last_tap_ns < int(d.tap_refractory_ms * 1_000_000.0):
            return
        self._begin_peak(sample, accel_norm, gyro_norm)

    def _run(self) -> None:
        while not self.stop_event.is_set():
            try:
                if self.client is None:
                    self._connect()
                samples = self._read_new_samples()
                for sample in samples:
                    self._process_sample(sample)
                self._flush_pending(time_monotonic_ns())
                self.stop_event.wait(self.cfg.source.poll_sleep_s)
            except Exception as exc:
                self.log(f"[imu_tap][WARN] {exc}; retrying")
                if self.client is not None:
                    try:
                        self.client.close()
                    except Exception:
                        pass
                    self.client = None
                self.stop_event.wait(1.0)


def time_monotonic_ns() -> int:
    # Python 3.7+ has time.monotonic_ns, but keep the import local to make this
    # module cheap to import when tap controls are disabled.
    import time
    return time.monotonic_ns()
