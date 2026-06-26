#!/usr/bin/env python3
"""Record IMU tap datasets from capture_service with guided prompts.

This is meant for calibrating temple-tap controls for XR glasses. It records raw
IMU samples plus labelled segment boundaries, so we can later tune a triple-tap
classifier and optional left/right side classifier.

Run while capture_service is already publishing imu0.
"""
from __future__ import annotations

import argparse
import csv
import json
import math
import os
import platform
import sys
import time
from dataclasses import asdict, dataclass
from datetime import datetime
from pathlib import Path
from typing import Any, Dict, Iterable, List, Optional, Sequence


def _expand_path(value: str) -> Path:
    return Path(value.replace("~", str(Path.home()), 1)).expanduser().resolve()


def _guess_project_root(script_path: Path) -> Path:
    # Preferred explicit value.
    env_root = os.environ.get("ROOT_PROJECT") or os.environ.get("XR_TRACKING_ROOT")
    if env_root:
        return _expand_path(env_root)

    # Common layout: <root>/xr_client/tools/xr_imu_tap_dataset_recorder.py
    candidates = [Path.cwd(), *script_path.resolve().parents]
    for cand in candidates:
        if (cand / "capture_client").exists() or (cand / "capture_service" / "capture_client").exists():
            return cand.resolve()
    # Fallback to current xr_tracking path convention.
    fallback = Path.home() / "src" / "xr_tracking"
    return fallback.resolve()


def _install_project_imports(project_root: Path) -> None:
    # Preferred layout: capture_client lives at project root.
    if (project_root / "capture_client").exists():
        sys.path.insert(0, str(project_root))

    # Legacy compatibility: older trees kept capture_client under capture_service.
    capture_service_root = project_root / "capture_service"
    if capture_service_root.exists():
        sys.path.insert(0, str(capture_service_root))


def _load_capture_client():
    try:
        from capture_client import CaptureClient  # type: ignore
    except Exception as exc:
        raise RuntimeError(
            "failed to import capture_client; run from xr_tracking root, set ROOT_PROJECT, "
            "or set PYTHONPATH=$ROOT_PROJECT"
        ) from exc
    return CaptureClient


@dataclass(frozen=True)
class SegmentSpec:
    label: str
    prompt: str
    duration_s: float
    side: str = "none"       # left/right/none/mixed/unknown
    kind: str = "neutral"    # quiet/tap/negative/etc.
    trials: int = 1
    rest_after_s: float = 1.0


def build_protocol(name: str) -> List[SegmentSpec]:
    if name == "quick":
        return [
            SegmentSpec("quiet_still", "Сиди спокойно, не трогай очки.", 6.0, kind="quiet"),
            SegmentSpec("left_triple_tap", "ТРИ стука по ЛЕВОЙ душке сразу после GO, потом замри.", 4.0, side="left", kind="tap", trials=5),
            SegmentSpec("right_triple_tap", "ТРИ стука по ПРАВОЙ душке сразу после GO, потом замри.", 4.0, side="right", kind="tap", trials=5),
            SegmentSpec("adjust_glasses", "Попрaвь очки рукой так, как обычно. Это NEGATIVE sample.", 5.0, kind="negative", trials=2),
            SegmentSpec("head_turns", "Поверни голову влево/вправо/вверх/вниз без стуков. NEGATIVE sample.", 6.0, kind="negative", trials=2),
        ]
    if name == "side_only":
        return [
            SegmentSpec("quiet_still", "Сиди спокойно, не трогай очки.", 8.0, kind="quiet"),
            SegmentSpec("left_triple_tap", "ТРИ стука по ЛЕВОЙ душке сразу после GO, потом замри.", 4.0, side="left", kind="tap", trials=12),
            SegmentSpec("right_triple_tap", "ТРИ стука по ПРАВОЙ душке сразу после GO, потом замри.", 4.0, side="right", kind="tap", trials=12),
        ]
    if name != "full":
        raise ValueError(f"unknown protocol: {name}")

    return [
        SegmentSpec("quiet_still", "Сиди спокойно, не трогай очки.", 8.0, kind="quiet", rest_after_s=1.5),
        SegmentSpec("left_single_tap", "ОДИН стук по ЛЕВОЙ душке сразу после GO. Нужно для формы импульса.", 3.0, side="left", kind="tap", trials=6),
        SegmentSpec("right_single_tap", "ОДИН стук по ПРАВОЙ душке сразу после GO. Нужно для формы импульса.", 3.0, side="right", kind="tap", trials=6),
        SegmentSpec("left_triple_tap", "ТРИ стука по ЛЕВОЙ душке сразу после GO, потом замри.", 4.0, side="left", kind="tap", trials=12),
        SegmentSpec("right_triple_tap", "ТРИ стука по ПРАВОЙ душке сразу после GO, потом замри.", 4.0, side="right", kind="tap", trials=12),
        SegmentSpec("mixed_tap_noise", "Сделай 1-2 случайных касания/лёгких удара НЕ triple-tap. NEGATIVE sample.", 5.0, side="mixed", kind="negative", trials=4),
        SegmentSpec("adjust_glasses", "Попрaвь очки рукой так, как обычно. NEGATIVE sample.", 5.0, kind="negative", trials=4),
        SegmentSpec("cable_touch", "Потрогай/слегка дёрни кабель, но не стучи по душке. NEGATIVE sample.", 5.0, kind="negative", trials=3),
        SegmentSpec("head_turns", "Поверни голову влево/вправо/вверх/вниз без стуков. NEGATIVE sample.", 7.0, kind="negative", trials=4),
        SegmentSpec("take_off_put_on", "Сними и надень очки или имитируй это движение. NEGATIVE sample.", 8.0, kind="negative", trials=2),
        SegmentSpec("quiet_final", "Снова сиди спокойно, не трогай очки.", 6.0, kind="quiet", rest_after_s=0.0),
    ]


class DatasetRecorder:
    def __init__(
        self,
        *,
        project_root: Path,
        out_dir: Path,
        transport: str,
        registry: str,
        tcp_host: str,
        tcp_port: int,
        imu_stream: str,
        beep: bool,
        poll_sleep_s: float,
    ) -> None:
        self.project_root = project_root
        self.out_dir = out_dir
        self.transport = transport
        self.registry = registry
        self.tcp_host = tcp_host
        self.tcp_port = tcp_port
        self.imu_stream = imu_stream
        self.beep = beep
        self.poll_sleep_s = poll_sleep_s
        self.client = None
        self.last_seq = 0
        self.dataset_id = out_dir.name
        self.run_start_mono_ns = time.monotonic_ns()

        self.samples_csv_path = out_dir / "imu_samples.csv"
        self.events_csv_path = out_dir / "events.csv"
        self.meta_path = out_dir / "metadata.json"
        self.samples_file = None
        self.events_file = None
        self.samples_writer = None
        self.events_writer = None

    def connect(self) -> None:
        CaptureClient = _load_capture_client()
        if self.transport == "shm":
            self.client = CaptureClient.from_shm_registry(self.registry, required_streams=[self.imu_stream])
        elif self.transport == "tcp":
            self.client = CaptureClient.from_tcp(self.tcp_host, self.tcp_port, required_streams=[self.imu_stream], subscribe_streams=[self.imu_stream])
        else:
            raise RuntimeError(f"unsupported transport: {self.transport}")

        streams = self.client.list_streams()
        if self.imu_stream not in streams:
            raise RuntimeError(f"IMU stream {self.imu_stream!r} not found; streams={list(streams.keys())}")
        self.last_seq = int(self.client.latest_sequence(self.imu_stream))

    def open_files(self, protocol: List[SegmentSpec], args: argparse.Namespace) -> None:
        self.out_dir.mkdir(parents=True, exist_ok=False)
        self.samples_file = self.samples_csv_path.open("w", newline="")
        self.events_file = self.events_csv_path.open("w", newline="")
        self.samples_writer = csv.DictWriter(
            self.samples_file,
            fieldnames=[
                "dataset_id",
                "segment_id",
                "label",
                "side",
                "kind",
                "trial_index",
                "sequence",
                "timestamp_ns",
                "monotonic_ns",
                "rel_s",
                "gx",
                "gy",
                "gz",
                "ax",
                "ay",
                "az",
                "gyro_norm",
                "accel_norm",
            ],
        )
        self.events_writer = csv.DictWriter(
            self.events_file,
            fieldnames=[
                "dataset_id",
                "segment_id",
                "label",
                "side",
                "kind",
                "trial_index",
                "prompt",
                "start_mono_ns",
                "end_mono_ns",
                "start_rel_s",
                "end_rel_s",
                "duration_s",
            ],
        )
        self.samples_writer.writeheader()
        self.events_writer.writeheader()

        streams_meta: Dict[str, Any] = {}
        try:
            streams = self.client.list_streams() if self.client is not None else {}
            for name, info in streams.items():
                raw = asdict(info)
                # Keep metadata compact enough for sharing.
                raw.pop("raw", None)
                streams_meta[name] = raw
        except Exception as exc:
            streams_meta = {"error": str(exc)}

        metadata = {
            "dataset_id": self.dataset_id,
            "created_unix_s": time.time(),
            "created_iso_local": datetime.now().isoformat(timespec="seconds"),
            "host": platform.node(),
            "platform": platform.platform(),
            "project_root": str(self.project_root),
            "transport": self.transport,
            "registry": self.registry,
            "tcp_host": self.tcp_host,
            "tcp_port": self.tcp_port,
            "imu_stream": self.imu_stream,
            "initial_latest_sequence": self.last_seq,
            "protocol_name": args.protocol,
            "protocol": [asdict(x) for x in protocol],
            "notes": "Labels are segment-level prompts; tap timing inside segment must be inferred from IMU impulses.",
            "streams": streams_meta,
            "argv": sys.argv,
        }
        self.meta_path.write_text(json.dumps(metadata, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")

    def close(self) -> None:
        for f in (self.samples_file, self.events_file):
            if f is not None:
                f.flush()
                f.close()
        if self.client is not None:
            self.client.close()

    def bell(self) -> None:
        if self.beep:
            print("\a", end="", flush=True)

    def _read_new_samples(self):
        assert self.client is not None
        latest = int(self.client.latest_sequence(self.imu_stream))
        if latest <= self.last_seq:
            return []
        # read_imu_range handles overwritten/missing slots by skipping None.
        samples = self.client.read_imu_range(self.imu_stream, self.last_seq + 1, latest)
        self.last_seq = latest
        return samples

    def drain_for(self, seconds: float) -> None:
        end = time.monotonic() + max(0.0, seconds)
        while time.monotonic() < end:
            self._read_new_samples()
            time.sleep(self.poll_sleep_s)

    def record_segment(self, *, segment_id: int, spec: SegmentSpec, trial_index: int) -> None:
        assert self.samples_writer is not None and self.events_writer is not None

        start_ns = time.monotonic_ns()
        start_rel_s = (start_ns - self.run_start_mono_ns) / 1e9
        end_deadline = time.monotonic() + spec.duration_s
        rows = 0

        while time.monotonic() < end_deadline:
            samples = self._read_new_samples()
            for s in samples:
                gx, gy, gz = s.gyro_rad_s
                ax, ay, az = s.accel_m_s2
                gyro_norm = math.sqrt(gx * gx + gy * gy + gz * gz)
                accel_norm = math.sqrt(ax * ax + ay * ay + az * az)
                rel_s = (int(s.monotonic_ns) - self.run_start_mono_ns) / 1e9
                self.samples_writer.writerow(
                    {
                        "dataset_id": self.dataset_id,
                        "segment_id": segment_id,
                        "label": spec.label,
                        "side": spec.side,
                        "kind": spec.kind,
                        "trial_index": trial_index,
                        "sequence": int(s.sequence),
                        "timestamp_ns": int(s.timestamp_ns),
                        "monotonic_ns": int(s.monotonic_ns),
                        "rel_s": f"{rel_s:.9f}",
                        "gx": f"{float(gx):.9f}",
                        "gy": f"{float(gy):.9f}",
                        "gz": f"{float(gz):.9f}",
                        "ax": f"{float(ax):.9f}",
                        "ay": f"{float(ay):.9f}",
                        "az": f"{float(az):.9f}",
                        "gyro_norm": f"{gyro_norm:.9f}",
                        "accel_norm": f"{accel_norm:.9f}",
                    }
                )
                rows += 1
            if self.samples_file is not None:
                self.samples_file.flush()
            time.sleep(self.poll_sleep_s)

        # Final small drain in case samples landed between loop iterations.
        for s in self._read_new_samples():
            gx, gy, gz = s.gyro_rad_s
            ax, ay, az = s.accel_m_s2
            gyro_norm = math.sqrt(gx * gx + gy * gy + gz * gz)
            accel_norm = math.sqrt(ax * ax + ay * ay + az * az)
            rel_s = (int(s.monotonic_ns) - self.run_start_mono_ns) / 1e9
            self.samples_writer.writerow(
                {
                    "dataset_id": self.dataset_id,
                    "segment_id": segment_id,
                    "label": spec.label,
                    "side": spec.side,
                    "kind": spec.kind,
                    "trial_index": trial_index,
                    "sequence": int(s.sequence),
                    "timestamp_ns": int(s.timestamp_ns),
                    "monotonic_ns": int(s.monotonic_ns),
                    "rel_s": f"{rel_s:.9f}",
                    "gx": f"{float(gx):.9f}",
                    "gy": f"{float(gy):.9f}",
                    "gz": f"{float(gz):.9f}",
                    "ax": f"{float(ax):.9f}",
                    "ay": f"{float(ay):.9f}",
                    "az": f"{float(az):.9f}",
                    "gyro_norm": f"{gyro_norm:.9f}",
                    "accel_norm": f"{accel_norm:.9f}",
                }
            )
            rows += 1

        end_ns = time.monotonic_ns()
        end_rel_s = (end_ns - self.run_start_mono_ns) / 1e9
        self.events_writer.writerow(
            {
                "dataset_id": self.dataset_id,
                "segment_id": segment_id,
                "label": spec.label,
                "side": spec.side,
                "kind": spec.kind,
                "trial_index": trial_index,
                "prompt": spec.prompt,
                "start_mono_ns": start_ns,
                "end_mono_ns": end_ns,
                "start_rel_s": f"{start_rel_s:.9f}",
                "end_rel_s": f"{end_rel_s:.9f}",
                "duration_s": f"{(end_ns - start_ns) / 1e9:.6f}",
            }
        )
        if self.events_file is not None:
            self.events_file.flush()
        print(f"[recorder] segment={segment_id} label={spec.label} trial={trial_index} rows={rows}", flush=True)


def countdown(seconds: int, beep: bool) -> None:
    for value in range(seconds, 0, -1):
        print(f"  {value}...", flush=True)
        if beep:
            print("\a", end="", flush=True)
        time.sleep(1.0)
    print("GO", flush=True)
    if beep:
        print("\a", end="", flush=True)


def parse_args(argv: Optional[Sequence[str]] = None) -> argparse.Namespace:
    ap = argparse.ArgumentParser(description="Guided IMU tap dataset recorder for XR glasses")
    ap.add_argument("--root", default=str(_guess_project_root(Path(__file__))), help="xr_tracking project root")
    ap.add_argument("--transport", choices=["shm", "tcp"], default="shm")
    ap.add_argument("--registry", default="/tmp/capture_service_streams.json")
    ap.add_argument("--tcp-host", default="127.0.0.1")
    ap.add_argument("--tcp-port", type=int, default=45660)
    ap.add_argument("--imu-stream", default="imu0")
    ap.add_argument("--out-dir", default="", help="Output dataset dir; default /tmp/xr_imu_tap_dataset_<timestamp>")
    ap.add_argument("--protocol", choices=["quick", "side_only", "full"], default="full")
    ap.add_argument("--countdown", type=int, default=3)
    ap.add_argument("--beep", type=int, default=1, help="Terminal bell on countdown/GO")
    ap.add_argument("--auto", action="store_true", help="Do not wait for Enter before every segment")
    ap.add_argument("--poll-sleep-ms", type=float, default=1.0)
    return ap.parse_args(argv)


def main(argv: Optional[Sequence[str]] = None) -> int:
    args = parse_args(argv)
    project_root = _expand_path(args.root)
    _install_project_imports(project_root)

    timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    out_dir = _expand_path(args.out_dir) if args.out_dir else Path(f"/tmp/xr_imu_tap_dataset_{timestamp}")
    protocol = build_protocol(args.protocol)

    recorder = DatasetRecorder(
        project_root=project_root,
        out_dir=out_dir,
        transport=args.transport,
        registry=args.registry,
        tcp_host=args.tcp_host,
        tcp_port=args.tcp_port,
        imu_stream=args.imu_stream,
        beep=bool(args.beep),
        poll_sleep_s=max(0.0005, float(args.poll_sleep_ms) / 1000.0),
    )

    print("=" * 80)
    print("XR IMU tap dataset recorder")
    print("=" * 80)
    print(f"project_root: {project_root}")
    print(f"out_dir:      {out_dir}")
    print(f"transport:    {args.transport}")
    print(f"registry:     {args.registry}")
    print(f"imu_stream:   {args.imu_stream}")
    print(f"protocol:     {args.protocol}")
    print()
    print("Перед стартом:")
    print("  1) capture_service должен уже работать и публиковать imu0")
    print("  2) очки лучше надеть как обычно")
    print("  3) стуки делай пальцем по душке, не по линзам/носовой части")
    print("  4) после каждого GO выполни ровно то, что написано в подсказке")
    print()

    try:
        recorder.connect()
        recorder.open_files(protocol, args)
        print("[recorder] connected; draining old IMU samples for 1s...")
        recorder.drain_for(1.0)

        segment_id = 0
        for spec in protocol:
            for trial in range(1, spec.trials + 1):
                segment_id += 1
                print("\n" + "-" * 80)
                print(f"SEGMENT {segment_id}: {spec.label} trial {trial}/{spec.trials}")
                print(f"duration: {spec.duration_s:.1f}s  side={spec.side} kind={spec.kind}")
                print(f"PROMPT: {spec.prompt}")
                print("-" * 80)
                if not args.auto:
                    input("Press Enter when ready...")
                countdown(max(0, int(args.countdown)), bool(args.beep))
                recorder.record_segment(segment_id=segment_id, spec=spec, trial_index=trial)
                if spec.rest_after_s > 0:
                    print(f"[recorder] rest {spec.rest_after_s:.1f}s")
                    recorder.drain_for(spec.rest_after_s)

        print("\n[recorder] DONE")
        print(f"dataset: {out_dir}")
        print(f"samples: {recorder.samples_csv_path}")
        print(f"events:  {recorder.events_csv_path}")
        print(f"meta:    {recorder.meta_path}")
        print("\nZip для отправки:")
        print(f"  cd {out_dir.parent} && zip -r {out_dir.name}.zip {out_dir.name}")
        return 0
    except KeyboardInterrupt:
        print("\n[recorder] interrupted", file=sys.stderr)
        print(f"partial dataset: {out_dir}", file=sys.stderr)
        return 130
    except Exception as exc:
        print(f"[recorder][ERROR] {exc}", file=sys.stderr)
        return 1
    finally:
        recorder.close()


if __name__ == "__main__":
    raise SystemExit(main())
