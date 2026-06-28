#!/usr/bin/env python3
"""Cross-platform XR backend launch client.

This command-line client is a configuration-driven replacement for the rigid
start_all_shm.sh cascade.  It starts capture/host-side services first, waits for
capture streams, runs the external startup gate, and only then starts tracking
and runtime backends.

New services/backends should be added in JSON config, not hard-coded here.
"""
from __future__ import annotations

import argparse
import json
import math
import os
import queue
import re
import signal
import subprocess
import sys
import threading
import time
from pathlib import Path
from typing import Any, Dict, List, Optional, Sequence, Tuple

try:
    from .common import env_truthy, expand_path, log, registry_has_stream
    from .config_reader import (
        ClientConfig,
        GateSpec,
        PrestartControlSpec,
        PrestartOptionSpec,
        ServiceSpec,
        WaitStream,
        default_config_dict,
        load_config,
        resolve_config_argument,
    )
    from .manual_controls import ManualControlPoller
    from .process_manager import RunningProcess, terminate_one_process_tree, terminate_running_processes
    from .tap_controls import ImuTapControlsSpec, ImuTapDetector, TapEvent
except ImportError:
    from common import env_truthy, expand_path, log, registry_has_stream
    from config_reader import (
        ClientConfig,
        GateSpec,
        PrestartControlSpec,
        PrestartOptionSpec,
        ServiceSpec,
        WaitStream,
        default_config_dict,
        load_config,
        resolve_config_argument,
    )
    from manual_controls import ManualControlPoller
    from process_manager import RunningProcess, terminate_one_process_tree, terminate_running_processes
    from tap_controls import ImuTapControlsSpec, ImuTapDetector, TapEvent

IS_POSIX = os.name == "posix"
IS_WINDOWS = os.name == "nt"

class Launcher:
    def __init__(self, cfg: ClientConfig, *, dry_run: bool = False) -> None:
        self.cfg = cfg
        self.dry_run = dry_run
        self.log_dir = Path(cfg.log_dir)
        self.running: List[RunningProcess] = []
        self.stop_requested = False
        self.control_events: "queue.Queue[TapEvent]" = queue.Queue()
        self.tap_detector: Optional[ImuTapDetector] = None
        self.manual_controls: Optional[ManualControlPoller] = None
        self._prestart_message_printed = False
        self._last_gate_visual_hint: Optional[Tuple[int, int]] = None
        self._last_gate_imu_hint: Optional[Tuple[int, int]] = None
        self._last_gate_hint_time_s = 0.0
        self._gate_visual_ready = False
        self._gate_imu_ready = False
        self._gate_quality_ready = False
        self._last_gate_quality_hint: Optional[Tuple[int, int, int]] = None
        self.restart_history: Dict[str, List[float]] = {}

    def fail(self, message: str) -> None:
        raise RuntimeError(message)

    def command_exists(self, command: Sequence[str]) -> bool:
        if not command:
            return False
        exe = command[0]
        if os.path.isabs(exe) or os.sep in exe or (IS_WINDOWS and "/" in exe):
            return Path(exe).exists()
        import shutil
        return shutil.which(exe) is not None

    def running_process_by_name(self, name: str) -> Optional[RunningProcess]:
        for item in self.running:
            if item.spec.name == name:
                return item
        return None

    def is_service_running(self, name: str) -> bool:
        item = self.running_process_by_name(name)
        return bool(item is not None and item.process.poll() is None)


    def print_prestart_message(self, spec: PrestartControlSpec) -> None:
        if self._prestart_message_printed or not spec.start_message:
            return
        self._prestart_message_printed = True
        print("", flush=True)
        for line in spec.start_message:
            print(line, flush=True)

    def find_prestart_option(self, spec: PrestartControlSpec, value: str) -> Optional[PrestartOptionSpec]:
        value_norm = str(value).strip().lower()
        for option in spec.options:
            names = [option.id, option.choice, option.label, *getattr(option, "aliases", [])]
            if value_norm in (str(name).strip().lower() for name in names):
                return option
        return None

    def select_prestart_option(self, spec: PrestartControlSpec) -> Optional[PrestartOptionSpec]:
        if not spec.options:
            return None
        selected = str(spec.selected_option).strip()
        if selected and selected.lower() not in ("", "prompt", "ask", "interactive"):
            option = self.find_prestart_option(spec, selected)
            if option is None:
                self.fail(f"Unknown prestart option {selected!r}")
            return option
        if not spec.prompt or not sys.stdin.isatty():
            option = self.find_prestart_option(spec, spec.default_option)
            return option or spec.options[0]

        default_option = self.find_prestart_option(spec, spec.default_option) or spec.options[0]
        while True:
            print("", flush=True)
            if spec.title:
                print(spec.title, flush=True)
            print(spec.prompt_message, flush=True)
            for option in spec.options:
                suffix = f" - {option.description}" if option.description else ""
                print(f"  {option.choice} - {option.label}{suffix}", flush=True)
            raw = input(f"Enter choice [{default_option.choice}]: ").strip()
            if not raw:
                raw = default_option.choice
            option = self.find_prestart_option(spec, raw)
            if option is not None:
                return option
            valid = ", ".join(option.choice for option in spec.options)
            print(f"Invalid choice. Use one of: {valid}.", flush=True)

    def wait_prestart_readiness(self, name: str, log_path: Path, patterns: Sequence[str], timeout_s: float, min_alive_s: float, status_interval_s: float) -> None:
        if not patterns and min_alive_s <= 0.0:
            return
        log(f"Waiting for {name} readiness in {log_path}")
        deadline = time.monotonic() + max(0.0, timeout_s) if timeout_s > 0.0 else None
        start = time.monotonic()
        last_status = start
        offset = 0
        while True:
            running = self.is_service_running(name)
            if not running:
                self.print_log_tail(log_path)
                self.fail(f"{name} stopped before readiness confirmation")
            if log_path.exists():
                try:
                    text = log_path.read_text(errors="replace")
                except Exception:
                    text = ""
                if offset < len(text):
                    chunk = text[offset:]
                    offset = len(text)
                    for line in chunk.splitlines():
                        if any(pattern in line for pattern in patterns):
                            log(f"OK: {name} ready: {line}")
                            return
                if patterns and any(pattern in text for pattern in patterns):
                    log(f"OK: {name} ready")
                    return
            elapsed = time.monotonic() - start
            if min_alive_s > 0.0 and elapsed >= min_alive_s:
                log(f"OK: {name} is still running after {elapsed:.1f}s; continuing without log readiness confirmation")
                return
            now = time.monotonic()
            if status_interval_s > 0.0 and now - last_status >= status_interval_s:
                size = log_path.stat().st_size if log_path.exists() else 0
                log(f"Still waiting for {name} readiness ({elapsed:.1f}s elapsed, log_size={size} bytes)")
                last_status = now
            if deadline is not None and now >= deadline:
                self.print_log_tail(log_path)
                self.fail(f"Timeout waiting for {name} readiness patterns: {list(patterns)}")
            time.sleep(0.25)

    def run_prestart_control(self) -> None:
        spec = self.cfg.prestart_control
        if spec is None or not spec.enabled:
            return
        self.print_prestart_message(spec)
        option = self.select_prestart_option(spec)
        if option is None:
            return
        log(f"Selected prestart option: {option.id} ({option.label})")
        if option.command:
            service = ServiceSpec(
                name=option.service_name or option.id,
                command=option.command,
                enabled=True,
                optional=False,
                cwd=option.cwd or self.cfg.root_project,
                env=option.env,
                start_delay_s=0.3,
                stop_timeout_s=option.stop_timeout_s,
            )
            started = self.start_service(service)
            if self.dry_run:
                log(f"DRY-RUN wait for {service.name} readiness")
            elif started:
                self.wait_prestart_readiness(
                    service.name,
                    self.log_dir / f"{service.name}.log",
                    option.wait_log_any,
                    option.wait_timeout_s,
                    option.readiness_min_alive_s,
                    option.readiness_status_interval_s,
                )
        if spec.pre_capture_wait_s > 0.0:
            log(f"Waiting {spec.pre_capture_wait_s:.1f}s before starting capture_service")
            if not self.dry_run:
                time.sleep(spec.pre_capture_wait_s)

    def clean_registries(self) -> None:
        if not self.cfg.clean_registries:
            return
        log("Cleaning old registry files")
        for registry in self.cfg.registries_to_clean:
            base = Path(registry)
            candidates = [base, Path(str(base) + ".lock")]
            candidates.extend(base.parent.glob(base.name + ".tmp*"))
            for path in candidates:
                try:
                    if path.exists():
                        if self.dry_run:
                            log(f"DRY-RUN remove {path}")
                        else:
                            path.unlink()
                except FileNotFoundError:
                    pass

    def start_log_limiter(self, item: RunningProcess) -> None:
        if self.cfg.log_max_bytes <= 0:
            return

        def worker() -> None:
            while not item.limiter_stop.is_set() and item.process.poll() is None:
                try:
                    if item.log_path.exists() and item.log_path.stat().st_size > self.cfg.log_max_bytes:
                        data = item.log_path.read_bytes()[-self.cfg.log_max_bytes:]
                        item.log_path.write_bytes(data)
                except Exception:
                    pass
                item.limiter_stop.wait(max(0.1, self.cfg.log_trim_interval_s))

        thread = threading.Thread(target=worker, name=f"{item.spec.name}-log-limiter", daemon=True)
        thread.start()

    def popen_kwargs(self) -> Dict[str, Any]:
        if IS_POSIX:
            return {"start_new_session": True}
        if IS_WINDOWS:
            return {"creationflags": subprocess.CREATE_NEW_PROCESS_GROUP}  # type: ignore[attr-defined]
        return {}

    def start_service(self, spec: ServiceSpec) -> bool:
        if not spec.enabled:
            log(f"Skipping {spec.name}: disabled")
            return False
        if not spec.command:
            if spec.optional:
                log(f"Skipping {spec.name}: empty optional command")
                return False
            self.fail(f"{spec.name} command is empty")
        if not self.dry_run and not self.command_exists(spec.command):
            if spec.optional:
                log(f"Skipping {spec.name}: optional executable not found: {spec.command[0]}")
                return False
            self.fail(f"{spec.name} executable not found: {spec.command[0]}")

        log_path = self.log_dir / f"{spec.name}.log"
        log(f"Starting {spec.name}")
        log(f"  log: {log_path}")
        log(f"  cmd: {' '.join(spec.command)}")
        if self.dry_run:
            return True

        env = os.environ.copy()
        env.update(spec.env)
        log_file = log_path.open("wb")
        try:
            proc = subprocess.Popen(
                spec.command,
                cwd=spec.cwd or self.cfg.root_project,
                env=env,
                stdout=log_file,
                stderr=subprocess.STDOUT,
                **self.popen_kwargs(),
            )
        except Exception:
            log_file.close()
            raise

        item = RunningProcess(spec=spec, process=proc, log_path=log_path, log_file=log_file)
        self.running.append(item)
        time.sleep(spec.start_delay_s)
        if proc.poll() is not None:
            self.print_log_tail(log_path)
            self.fail(f"{spec.name} exited immediately with code {proc.returncode}")
        if spec.name == "basalt_vio":
            log(
                "IMPORTANT: Basalt VIO is initializing. For correct positioning, "
                "gently rotate or move your head for a couple of seconds. "
                "Avoid fast turns, shaking, or large movements.",
                color="red",
            )
        self.start_log_limiter(item)
        return True

    def print_log_tail(self, path: Path, lines: int = 80) -> None:
        try:
            if not path.exists():
                return
            print(f"----- tail: {path} -----", file=sys.stderr)
            content = path.read_text(errors="replace").splitlines()[-lines:]
            for line in content:
                print(line, file=sys.stderr)
            print("-----------------------", file=sys.stderr)
        except Exception:
            pass

    def is_running(self, name: str) -> bool:
        for item in self.running:
            if item.spec.name == name:
                return item.process.poll() is None
        return False

    def wait_stream(self, producer_name: str, wait: WaitStream) -> None:
        log(f"Waiting for stream '{wait.stream}' in {wait.registry}")
        start = time.monotonic()
        while True:
            if registry_has_stream(wait.registry, wait.stream):
                log(f"OK: stream '{wait.stream}' is registered")
                return
            if not self.is_running(producer_name):
                for item in self.running:
                    if item.spec.name == producer_name:
                        self.print_log_tail(item.log_path)
                self.fail(f"{producer_name} stopped before stream '{wait.stream}' appeared")
            if time.monotonic() - start >= wait.timeout_s:
                for item in self.running:
                    if item.spec.name == producer_name:
                        self.print_log_tail(item.log_path)
                self.fail(f"Timeout waiting for stream '{wait.stream}' in {wait.registry}")
            time.sleep(0.25)

    def wait_for_service_streams(self, spec: ServiceSpec) -> None:
        if self.dry_run:
            for wait in spec.wait_streams:
                log(f"DRY-RUN wait for stream '{wait.stream}' in {wait.registry}")
            if spec.ready_message:
                log(spec.ready_message)
            return
        for wait in spec.wait_streams:
            self.wait_stream(spec.name, wait)
        if spec.ready_message:
            log(spec.ready_message)


    def _describe_control_action(self, event_name: str, action: Dict[str, Any]) -> str:
        description = str(action.get("description") or "").strip()
        if description:
            return description
        action_type = str(action.get("type", "none"))
        if action_type == "restart_services":
            services = ", ".join(self._service_names_from_action(action))
            return f"Restart services: {services}" if services else "Restart configured services"
        if action_type == "toggle_service":
            service = str(action.get("service") or "")
            return f"Start/stop {service}" if service else "Start/stop configured service"
        if action_type == "toggle_exclusive_services":
            primary = str(action.get("primary_service") or action.get("sixdof_service") or "primary")
            secondary = str(action.get("secondary_service") or action.get("threedof_service") or "secondary")
            return f"Switch between {primary} and {secondary}"
        if action_type == "recenter_3dof":
            return "Recenter the 3DoF yaw origin"
        if action_type in ("toggle_imu_tap_controls", "toggle_shake_controls"):
            return "Enable/disable IMU tap controls for this session"
        if action_type == "external_command":
            return "Run configured external command"
        if action_type in ("none", "noop", "no-op"):
            return "No operation"
        return action_type

    def log_imu_tap_actions(self, controls: ImuTapControlsSpec) -> None:
        actions = controls.actions or {}
        imu_actions = [(name, action) for name, action in sorted(actions.items()) if not name.startswith("manual-")]
        manual_actions = [(name, action) for name, action in sorted(actions.items()) if name.startswith("manual-")]

        log("Configured IMU tap actions:")
        if not imu_actions:
            log("  none")
        for name, action in imu_actions:
            log(f"  {name}: {self._describe_control_action(name, action)}")

        if manual_actions:
            log("Configured manual control actions:")
            for name, action in manual_actions:
                log(f"  {name}: {self._describe_control_action(name, action)}")

    def start_imu_tap_controls(self) -> None:
        controls = self.cfg.imu_tap_controls
        if controls is None or not controls.enabled:
            log("IMU tap controls disabled")
            return
        if self.tap_detector is not None:
            log("IMU tap controls already enabled")
            return
        if self.dry_run:
            log("DRY-RUN start IMU tap controls")
            self.log_imu_tap_actions(controls)
            return
        self.tap_detector = ImuTapDetector(controls, self.cfg.root_project, self.control_events, logger=log)
        self.tap_detector.start()
        log(
            "IMU tap controls enabled: "
            f"threshold={controls.detector.tap_accel_threshold} "
            f"double={controls.detector.double_min_interval_ms}-{controls.detector.double_max_interval_ms}ms "
            f"triple={controls.detector.triple_min_interval_ms}-{controls.detector.triple_max_interval_ms}ms "
            f"quadruple={controls.detector.quadruple_min_interval_ms}-{controls.detector.quadruple_max_interval_ms}ms "
            f"cooldown={controls.detector.cooldown_ms}ms"
        )
        self.log_imu_tap_actions(controls)

    def stop_imu_tap_controls(self) -> None:
        if self.tap_detector is None:
            log("IMU tap controls already disabled")
            return
        self.tap_detector.stop()
        self.tap_detector = None
        log("IMU tap controls disabled for this session")

    def service_spec_by_name(self, name: str) -> Optional[ServiceSpec]:
        for spec in self.pre_gate_services_iter() + self.post_gate_services_iter() + self.foreground_services_iter():
            if spec.name == name:
                return spec
        return None

    def pre_gate_services_iter(self) -> List[ServiceSpec]:
        return list(self.cfg.pre_gate_services)

    def post_gate_services_iter(self) -> List[ServiceSpec]:
        return list(self.cfg.post_gate_services)

    def foreground_services_iter(self) -> List[ServiceSpec]:
        return list(self.cfg.foreground_services)

    def running_items_by_name(self, names: Sequence[str]) -> List[RunningProcess]:
        wanted = set(names)
        return [item for item in self.running if item.spec.name in wanted]

    def stop_service_names(self, names: Sequence[str]) -> None:
        wanted = set(names)
        items = [item for item in self.running if item.spec.name in wanted]
        if not items:
            log(f"No running services to stop for: {', '.join(names)}")
            return
        log(f"Stopping services: {', '.join(item.spec.name for item in items)}")
        for item in reversed(items):
            item.limiter_stop.set()
            proc = item.process
            if proc.poll() is not None:
                continue
            try:
                if IS_POSIX:
                    os.killpg(proc.pid, signal.SIGTERM)
                elif IS_WINDOWS:
                    proc.send_signal(signal.CTRL_BREAK_EVENT)  # type: ignore[attr-defined]
                else:
                    proc.terminate()
            except Exception:
                try:
                    proc.terminate()
                except Exception:
                    pass
        deadline = time.monotonic() + max((x.spec.stop_timeout_s for x in items), default=1.0)
        while time.monotonic() < deadline:
            if all(item.process.poll() is not None for item in items):
                break
            time.sleep(0.1)
        for item in reversed(items):
            proc = item.process
            if proc.poll() is None:
                try:
                    if IS_POSIX:
                        os.killpg(proc.pid, signal.SIGKILL)
                    else:
                        proc.kill()
                except Exception:
                    pass
            try:
                item.log_file.close()
            except Exception:
                pass
        self.running = [item for item in self.running if item not in items]

    def clean_registry_paths(self, paths: Sequence[str]) -> None:
        if not paths:
            return
        log("Cleaning selected registry files")
        for registry in paths:
            expanded = expand_path(str(registry), self.cfg.root_project)
            base = Path(expanded)
            candidates = [base, Path(str(base) + ".lock")]
            candidates.extend(base.parent.glob(base.name + ".tmp*"))
            for path in candidates:
                try:
                    if path.exists():
                        log(f"  remove {path}")
                        path.unlink()
                except FileNotFoundError:
                    pass

    def _event_prefix(self, event: TapEvent) -> str:
        return "[manual]" if event.side == "manual" or event.name.startswith("manual-") else "[imu_tap]"

    def _service_names_from_action(self, action: Dict[str, Any], key: str = "services") -> List[str]:
        raw = action.get(key, [])
        if isinstance(raw, str):
            return [raw]
        return [str(x) for x in raw]

    def remove_registry_streams(self, registry: str, streams: Sequence[str]) -> None:
        stream_names = {str(x) for x in streams if str(x)}
        if not stream_names:
            return
        path = Path(expand_path(str(registry), self.cfg.root_project))
        if self.dry_run:
            log(f"DRY-RUN remove streams {sorted(stream_names)} from {path}")
            return
        try:
            data = json.loads(path.read_text())
        except FileNotFoundError:
            return
        except Exception as exc:
            log(f"[xr_control][WARN] cannot read registry {path}: {exc}")
            return

        changed = False

        def remove_from_obj(obj: Any) -> Any:
            nonlocal changed
            if isinstance(obj, dict):
                for stream in list(stream_names):
                    if stream in obj:
                        obj.pop(stream, None)
                        changed = True
                streams_obj = obj.get("streams")
                if isinstance(streams_obj, dict):
                    for stream in list(stream_names):
                        if stream in streams_obj:
                            streams_obj.pop(stream, None)
                            changed = True
                elif isinstance(streams_obj, list):
                    before = len(streams_obj)
                    obj["streams"] = [
                        item for item in streams_obj
                        if not (
                            isinstance(item, dict)
                            and any(item.get(k) in stream_names for k in ("stream", "stream_id", "stream_name", "name", "id"))
                        )
                    ]
                    changed = changed or len(obj["streams"]) != before
                for key, value in list(obj.items()):
                    obj[key] = remove_from_obj(value)
                return obj
            if isinstance(obj, list):
                before = len(obj)
                filtered = []
                for item in obj:
                    if isinstance(item, dict) and any(item.get(k) in stream_names for k in ("stream", "stream_id", "stream_name", "name", "id")):
                        changed = True
                        continue
                    filtered.append(remove_from_obj(item))
                changed = changed or len(filtered) != before
                return filtered
            return obj

        data = remove_from_obj(data)
        if not changed:
            return
        tmp = path.with_name(path.name + ".tmp.xr_client")
        try:
            tmp.write_text(json.dumps(data, indent=2, ensure_ascii=False) + "\n")
            tmp.replace(path)
            log(f"Removed streams {sorted(stream_names)} from {path}")
        except Exception as exc:
            log(f"[xr_control][WARN] cannot update registry {path}: {exc}")
            try:
                tmp.unlink()
            except Exception:
                pass

    def clean_stream_entries(self, items: Sequence[Dict[str, Any]]) -> None:
        for item in items or []:
            registry = item.get("registry")
            streams = item.get("streams", [])
            if isinstance(streams, str):
                streams = [streams]
            if registry:
                self.remove_registry_streams(str(registry), [str(x) for x in streams])

    def _start_service_by_name_for_action(self, name: str, event: TapEvent, *, wait_streams: bool = True) -> bool:
        prefix = self._event_prefix(event)
        spec = self.service_spec_by_name(name)
        if spec is None:
            log(f"{prefix}[WARN] no service spec named {name!r}")
            return False
        if self.is_service_running(name):
            log(f"{prefix} {name} is already running")
            return True
        try:
            started = self.start_service(spec)
            if started and wait_streams:
                self.wait_for_service_streams(spec)
            return started
        except Exception as exc:
            if spec.optional:
                log(f"{prefix}[WARN] optional service {name} failed to start: {exc}")
                self.stop_service_names([name])
                return False
            raise

    def restart_services_action(self, action: Dict[str, Any], event: TapEvent) -> None:
        prefix = self._event_prefix(event)
        service_names = self._service_names_from_action(action)
        if not service_names:
            log(f"{prefix} action {event.name}: restart_services has empty services list")
            return
        if bool(action.get("running_only", False)):
            service_names = [name for name in service_names if self.is_service_running(name)]
        if not service_names:
            log(f"{prefix} action {event.name}: no matching running services to restart")
            return
        log(f"{prefix} action {event.name}: restart services {service_names}")
        self.stop_service_names(service_names)
        self.clean_registry_paths([str(x) for x in action.get("clean_registries", [])])
        self.clean_stream_entries([dict(x) for x in action.get("clean_streams", [])])
        if bool(action.get("run_gate", False)):
            self.run_gate(self.cfg.gate)
        wait_streams = bool(action.get("wait_streams", True))
        for name in service_names:
            self._start_service_by_name_for_action(name, event, wait_streams=wait_streams)
        log(f"{prefix} action {event.name}: restart complete")

    def toggle_service_action(self, action: Dict[str, Any], event: TapEvent) -> None:
        prefix = self._event_prefix(event)
        service_name = str(action.get("service") or "")
        if not service_name:
            log(f"{prefix} action {event.name}: toggle_service has empty service")
            return
        if self.is_service_running(service_name):
            log(f"{prefix} action {event.name}: stop {service_name}")
            self.stop_service_names([service_name])
            self.clean_registry_paths([str(x) for x in action.get("clean_registries_on_stop", [])])
            self.clean_stream_entries([dict(x) for x in action.get("clean_streams_on_stop", [])])
            return
        log(f"{prefix} action {event.name}: start {service_name}")
        self.clean_registry_paths([str(x) for x in action.get("clean_registries_on_start", [])])
        self.clean_stream_entries([dict(x) for x in action.get("clean_streams_on_start", [])])
        self._start_service_by_name_for_action(service_name, event, wait_streams=bool(action.get("wait_streams", True)))

    def toggle_exclusive_services_action(self, action: Dict[str, Any], event: TapEvent) -> None:
        prefix = self._event_prefix(event)
        primary = str(action.get("primary_service") or action.get("sixdof_service") or "basalt_vio")
        secondary = str(action.get("secondary_service") or action.get("threedof_service") or "imu_3dof_backend")
        primary_running = self.is_service_running(primary)
        secondary_running = self.is_service_running(secondary)

        if secondary_running:
            stop_name = secondary
            start_name = primary
            stop_clean = [dict(x) for x in action.get("secondary_clean_streams_on_stop", [])]
            start_clean = [dict(x) for x in action.get("primary_clean_streams_on_start", [])]
            log(f"{prefix} action {event.name}: switch {secondary} -> {primary}")
        elif primary_running:
            stop_name = primary
            start_name = secondary
            stop_clean = [dict(x) for x in action.get("primary_clean_streams_on_stop", [])]
            start_clean = [dict(x) for x in action.get("secondary_clean_streams_on_start", [])]
            log(f"{prefix} action {event.name}: switch {primary} -> {secondary}")
        else:
            start_name = str(action.get("start_when_none") or secondary)
            stop_name = ""
            start_clean = [dict(x) for x in action.get("secondary_clean_streams_on_start", [])]
            stop_clean = []
            log(f"{prefix} action {event.name}: neither {primary} nor {secondary} is running; start {start_name}")

        wait_streams = bool(action.get("wait_streams", True))
        make_before_break = bool(action.get("make_before_break", True))

        if stop_name and make_before_break:
            # Do not create a pose-output gap while switching HMD backends.
            # Start the target backend, wait until its stream is registered,
            # and only then stop the old backend / remove its stream entry.
            # This prevents SteamVR from entering a grey/standby state during
            # 6DoF <-> 3DoF switching.
            self.clean_stream_entries(start_clean)
            started = self._start_service_by_name_for_action(start_name, event, wait_streams=wait_streams)
            if not started:
                log(
                    f"{prefix} action {event.name}: [WARN] failed to start {start_name}; "
                    f"keeping {stop_name} running"
                )
                return
            self.stop_service_names([stop_name])
            self.clean_stream_entries(stop_clean)
            return

        if stop_name:
            self.stop_service_names([stop_name])
            self.clean_stream_entries(stop_clean)
        self.clean_stream_entries(start_clean)
        self._start_service_by_name_for_action(start_name, event, wait_streams=wait_streams)

    def recenter_3dof_action(self, action: Dict[str, Any], event: TapEvent) -> None:
        prefix = self._event_prefix(event)
        service_name = str(action.get("service", "imu_3dof_backend"))
        if bool(action.get("only_if_service_running", True)) and not self.is_service_running(service_name):
            log(f"{prefix} action {event.name}: ignored; {service_name} is not running")
            return
        control_file = Path(expand_path(str(action.get("control_file", "/tmp/xr_backend_control.json")), self.cfg.root_project))
        counter_key = str(action.get("counter_key", "imu_3dof_recenter_counter"))
        if self.dry_run:
            log(f"DRY-RUN increment {counter_key} in {control_file}")
            return
        try:
            data = json.loads(control_file.read_text()) if control_file.exists() else {}
            if not isinstance(data, dict):
                data = {}
        except Exception:
            data = {}
        data[counter_key] = int(data.get(counter_key, 0)) + 1
        control_file.parent.mkdir(parents=True, exist_ok=True)
        tmp = control_file.with_name(control_file.name + ".tmp.xr_client")
        tmp.write_text(json.dumps(data, indent=2, ensure_ascii=False) + "\n")
        tmp.replace(control_file)
        log(f"{prefix} action {event.name}: recenter requested; {counter_key}={data[counter_key]}")

    def external_command_action(self, action: Dict[str, Any], event: TapEvent) -> None:
        raw_command = action.get("command") or []
        if isinstance(raw_command, str):
            raw_command = [raw_command]
        command = [
            expand_path(str(x), self.cfg.root_project)
            .replace("{event}", event.name)
            .replace("{side}", event.side)
            .replace("{confidence}", f"{event.confidence:.3f}")
            for x in raw_command
        ]
        if not command:
            log(f"{self._event_prefix(event)} action {event.name}: external_command has empty command")
            return
        cwd = expand_path(str(action.get("cwd", self.cfg.root_project)), self.cfg.root_project)
        env = os.environ.copy()
        env.update({str(k): expand_path(str(v), self.cfg.root_project) for k, v in dict(action.get("env", {})).items()})
        env.update({
            "XR_TAP_EVENT": event.name,
            "XR_TAP_SIDE": event.side,
            "XR_TAP_CONFIDENCE": f"{event.confidence:.3f}",
        })
        wait = bool(action.get("wait", True))
        log(f"{self._event_prefix(event)} action {event.name}: external command {' '.join(command)}")
        if self.dry_run:
            return
        if wait:
            code = subprocess.call(command, cwd=cwd, env=env)
            if code != 0 and not bool(action.get("ignore_failure", False)):
                self.fail(f"external command for {event.name} failed with code {code}")
        else:
            subprocess.Popen(command, cwd=cwd, env=env, **self.popen_kwargs())

    def toggle_imu_tap_controls_action(self, action: Dict[str, Any], event: TapEvent) -> None:
        prefix = self._event_prefix(event)
        controls = self.cfg.imu_tap_controls
        if controls is None:
            log(f"{prefix} action {event.name}: IMU tap controls are not configured")
            return
        if self.tap_detector is not None:
            log(f"{prefix} action {event.name}: disable IMU tap controls")
            self.stop_imu_tap_controls()
            controls.enabled = False
            return
        log(f"{prefix} action {event.name}: enable IMU tap controls")
        controls.enabled = True
        if "debug" in action:
            controls.debug = bool(action.get("debug"))
        self.start_imu_tap_controls()

    def handle_tap_event(self, event: TapEvent) -> None:
        controls = self.cfg.imu_tap_controls
        if controls is None:
            return
        action = controls.actions.get(event.name) or controls.actions.get("*")
        if action is None:
            log(f"{self._event_prefix(event)} event={event.name} side={event.side} action=none/unconfigured")
            return
        action_type = str(action.get("type", "none"))
        log(f"{self._event_prefix(event)} event={event.name} side={event.side} confidence={event.confidence:.2f} action={action_type}")
        if action_type in ("none", "noop", "no-op"):
            return
        if action_type == "restart_services":
            self.restart_services_action(action, event)
            return
        if action_type == "toggle_service":
            self.toggle_service_action(action, event)
            return
        if action_type == "toggle_exclusive_services":
            self.toggle_exclusive_services_action(action, event)
            return
        if action_type == "recenter_3dof":
            self.recenter_3dof_action(action, event)
            return
        if action_type == "external_command":
            self.external_command_action(action, event)
            return
        if action_type in ("toggle_imu_tap_controls", "toggle_shake_controls"):
            self.toggle_imu_tap_controls_action(action, event)
            return
        log(f"{self._event_prefix(event)}[WARN] unknown action type {action_type!r} for {event.name}")

    def start_manual_control_loop(self) -> None:
        if self.manual_controls is not None:
            return
        if not ManualControlPoller.available():
            return
        self.manual_controls = ManualControlPoller()
        self.manual_controls.print_menu()

    def drain_control_events(self) -> None:
        if self.manual_controls is not None:
            for event in self.manual_controls.poll_events():
                self.control_events.put(event)
            if self.manual_controls.stop_requested:
                self.stop_requested = True
        while True:
            try:
                event = self.control_events.get_nowait()
            except queue.Empty:
                return
            self.handle_tap_event(event)


    def _emit_gate_hint(self, gate: GateSpec, message: str, *, force: bool = False) -> None:
        now = time.monotonic()
        interval = max(0.0, gate.progress_interval_s)
        if not force and interval > 0.0 and now - self._last_gate_hint_time_s < interval:
            return
        self._last_gate_hint_time_s = now
        log(message)

    def _format_gate_hint(self, template: str, *, good: int, target: int, percent: float) -> str:
        try:
            return template.format(good=good, target=target, percent=percent)
        except Exception:
            return f"Startup check: {percent:.0f}% ({good}/{target})."

    def handle_gate_output_line(self, gate: GateSpec, line: str) -> None:
        if not gate.user_hints:
            return

        quality_match = re.search(
            r"stream quality gate:\s*elapsed=([0-9.]+)/([0-9.]+)s\s+seen=(\d+)\s+defective=(\d+)/(\d+)",
            line,
        )
        if quality_match:
            elapsed = float(quality_match.group(1))
            total = max(0.001, float(quality_match.group(2)))
            seen = int(quality_match.group(3))
            defective = int(quality_match.group(4))
            max_defective = int(quality_match.group(5))
            hint_key = (seen, defective, max_defective)
            if hint_key == self._last_gate_quality_hint:
                return
            self._last_gate_quality_hint = hint_key
            percent = min(100.0, max(0.0, 100.0 * elapsed / total))
            try:
                message = gate.quality_progress_template.format(
                    percent=percent,
                    seen=seen,
                    defective=defective,
                    max_defective=max_defective,
                )
            except Exception:
                message = f"Startup stream quality: {percent:.0f}% — defective frames {defective}/{max_defective}."
            self._emit_gate_hint(gate, message)
            return

        quality_result_match = re.search(
            r"stream quality gate result:.*seen=(\d+)\s+defective=(\d+)/(\d+).*passed=true",
            line,
        )
        if quality_result_match:
            self._gate_quality_ready = True
            self._emit_gate_hint(gate, gate.quality_ready_template, force=True)
            return

        visual_match = re.search(r"startup visual gate:\s*seen=(\d+)\s+good=(\d+)/(\d+)", line)
        if visual_match:
            good = int(visual_match.group(2))
            target = max(1, int(visual_match.group(3)))
            hint_key = (good, target)
            if hint_key == self._last_gate_visual_hint:
                return
            self._last_gate_visual_hint = hint_key
            percent = min(100.0, max(0.0, 100.0 * float(good) / float(target)))
            visual_ready_now = good >= target
            force = visual_ready_now and not self._gate_visual_ready
            self._gate_visual_ready = self._gate_visual_ready or visual_ready_now
            template = gate.visual_ready_template if visual_ready_now else gate.visual_progress_template
            self._emit_gate_hint(
                gate,
                self._format_gate_hint(template, good=good, target=target, percent=percent),
                force=force,
            )
            return

        imu_match = re.search(r"startup IMU gate:\s*seen=(\d+)\s+good=(\d+)/(\d+)", line)
        if imu_match:
            good = int(imu_match.group(2))
            target = max(1, int(imu_match.group(3)))
            hint_key = (good, target)
            if hint_key == self._last_gate_imu_hint:
                return
            self._last_gate_imu_hint = hint_key
            percent = min(100.0, max(0.0, 100.0 * float(good) / float(target)))
            imu_ready_now = good >= target
            was_imu_ready = self._gate_imu_ready
            self._gate_imu_ready = self._gate_imu_ready or imu_ready_now
            # Do not print the final IMU-ready message while the visual gate is still
            # failing. Otherwise the UI can show a misleading "Startup check: ready."
            # even though cameras are not ready yet. The timeout countdown remains
            # visible during that period.
            if not self._gate_visual_ready:
                return
            force = imu_ready_now and not was_imu_ready
            template = gate.imu_ready_template if imu_ready_now else gate.imu_progress_template
            self._emit_gate_hint(
                gate,
                self._format_gate_hint(template, good=good, target=target, percent=percent),
                force=force,
            )
            return

    def _release_startup_gate_process(self, process: subprocess.Popen) -> None:
        """Release a one-shot startup gate process without blocking restart actions."""
        def reap() -> None:
            try:
                terminate_one_process_tree(process, graceful_wait_s=0.75, kill_wait_s=1.0)
            except Exception:
                pass

        threading.Thread(target=reap, name="startup-gate-reaper", daemon=True).start()

    def _emit_gate_timeout_countdown(self, gate: GateSpec, deadline_s: Optional[float], *, force: bool = False) -> None:
        if deadline_s is None:
            return
        now = time.monotonic()
        remaining_s = max(0.0, deadline_s - now)
        interval = max(0.1, float(getattr(gate, "timeout_status_interval_s", 1.0)))
        last = getattr(self, "_last_gate_timeout_hint_time_s", 0.0)
        remaining_i = int(math.ceil(remaining_s))
        last_remaining = getattr(self, "_last_gate_timeout_remaining_i", None)
        if not force and now - last < interval and remaining_i == last_remaining:
            return
        self._last_gate_timeout_hint_time_s = now
        self._last_gate_timeout_remaining_i = remaining_i
        log(f"Startup gate timeout in {remaining_i}s", color="yellow")

    def _restart_services_for_gate_retry(self, gate: GateSpec) -> None:
        names = list(getattr(gate, "restart_services_on_failure", []) or [])
        if not names:
            return
        log(f"Startup gate recovery: restarting services: {', '.join(names)}", color="yellow")
        self.stop_service_names(names)
        self.clean_registry_paths(getattr(gate, "restart_clean_registries", []) or [])
        for name in names:
            spec = self.service_spec_by_name(name)
            if spec is None:
                self.fail(f"Startup gate recovery service not found: {name}")
            started = self.start_service(spec)
            if started:
                self.wait_for_service_streams(spec)

    def _prompt_after_gate_failure(self, gate: GateSpec, reason: str, max_attempts: int) -> str:
        log(
            f"Startup gate failed after {max_attempts} attempt(s): {reason}. "
            "Press R then Enter to restart capture/check again, or Enter to start without gate.",
            color="yellow",
        )
        if not gate.timeout_prompt or not sys.stdin.isatty():
            log("Startup gate failure prompt is unavailable; starting without gate", color="yellow")
            return "skip"
        while True:
            raw = input("Startup gate action [Enter=start without gate, R=restart/check again]: ").strip().lower()
            if raw == "":
                return "skip"
            if raw == "r":
                return "retry"
            print("Invalid choice. Press Enter to start without gate, or R then Enter to restart/check again.", flush=True)

    def _prompt_after_gate_timeout(self, gate: GateSpec, timeout_s: float) -> str:
        log(
            f"Startup gate timed out after {timeout_s:.0f}s: conditions are not suitable. "
            "Press R then Enter to retry, or Enter to start without gate.",
            color="yellow",
        )
        if not gate.timeout_prompt or not sys.stdin.isatty():
            log("Startup gate timeout prompt is unavailable; starting without gate", color="yellow")
            return "skip"
        while True:
            raw = input("Startup gate action [Enter=start without gate, R=retry]: ").strip().lower()
            if raw == "":
                return "skip"
            if raw == "r":
                return "retry"
            print("Invalid choice. Press Enter to start without gate, or R then Enter to retry.", flush=True)

    def _read_gate_stdout(self, process: subprocess.Popen, out_queue: "queue.Queue[Optional[str]]") -> None:
        try:
            assert process.stdout is not None
            for line in process.stdout:
                out_queue.put(line)
        except Exception as exc:
            out_queue.put(f"[xr_backend_client][gate-reader-error] {exc}\n")
        finally:
            out_queue.put(None)

    def run_gate(self, gate: Optional[GateSpec]) -> None:
        if gate is None or not gate.enabled:
            log("Startup gate disabled")
            return
        if not gate.command:
            self.fail("Startup gate command is empty")
        if not self.dry_run and not self.command_exists(gate.command):
            self.fail(f"Startup gate executable not found: {gate.command[0]}")

        log_path = self.log_dir / f"{gate.log_name}.log"
        max_attempts = max(1, int(getattr(gate, "max_attempts", 1)))
        attempt = 0
        while True:
            attempt += 1
            self._last_gate_visual_hint = None
            self._last_gate_imu_hint = None
            self._last_gate_hint_time_s = 0.0
            self._last_gate_timeout_hint_time_s = 0.0
            self._last_gate_timeout_remaining_i = None
            self._gate_visual_ready = False
            self._gate_imu_ready = False
            self._gate_quality_ready = False
            self._last_gate_quality_hint = None
            timeout_s = max(0.0, float(getattr(gate, "timeout_s", 30.0)))
            deadline_s = time.monotonic() + timeout_s if timeout_s > 0.0 else None

            log("Running startup gate" if attempt == 1 else "Retrying startup gate")
            log(f"  log: {log_path}")
            log(f"  cmd: {' '.join(gate.command)}")
            if self.dry_run:
                return

            env = os.environ.copy()
            env.update(gate.env)
            gate_ready_from_output = False
            gate_pass_line_seen = False
            gate_timed_out = False
            code: Optional[int] = None
            process: Optional[subprocess.Popen] = None
            line_queue: "queue.Queue[Optional[str]]" = queue.Queue()

            with log_path.open("wb" if attempt == 1 else "ab") as log_file:
                if attempt > 1:
                    log_file.write(f"\n[xr_backend_client] --- startup gate retry {attempt} ---\n".encode("utf-8"))
                process = subprocess.Popen(
                    gate.command,
                    cwd=gate.cwd or self.cfg.root_project,
                    env=env,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.STDOUT,
                    text=True,
                    bufsize=1,
                    **self.popen_kwargs(),
                )
                threading.Thread(
                    target=self._read_gate_stdout,
                    args=(process, line_queue),
                    name="startup-gate-stdout-reader",
                    daemon=True,
                ).start()
                self._emit_gate_timeout_countdown(gate, deadline_s, force=True)

                while True:
                    now = time.monotonic()
                    if deadline_s is not None and now >= deadline_s:
                        gate_timed_out = True
                        break
                    timeout = 0.2
                    if deadline_s is not None:
                        timeout = min(timeout, max(0.0, deadline_s - now))
                    try:
                        line = line_queue.get(timeout=timeout)
                    except queue.Empty:
                        self._emit_gate_timeout_countdown(gate, deadline_s)
                        continue

                    if line is None:
                        break

                    if gate.debug_output:
                        sys.stdout.write(line)
                        sys.stdout.flush()
                    self.handle_gate_output_line(gate, line)
                    log_file.write(line.encode("utf-8", errors="replace"))
                    log_file.flush()

                    if "[xr_startup_gate] startup gate passed:" in line:
                        gate_pass_line_seen = True

                    if gate_pass_line_seen or (self._gate_visual_ready and self._gate_imu_ready):
                        gate_ready_from_output = True
                        self._release_startup_gate_process(process)
                        code = 0
                        break

                    self._emit_gate_timeout_countdown(gate, deadline_s)

                if gate_timed_out:
                    self._release_startup_gate_process(process)
                    code = None
                elif code is None:
                    try:
                        code = process.wait(timeout=0.5)
                    except subprocess.TimeoutExpired:
                        self._release_startup_gate_process(process)
                        code = 1

            if gate_ready_from_output:
                log("Startup gate passed")
                return

            if gate_timed_out:
                reason = f"timeout after {timeout_s:.0f}s"
            else:
                if code is None:
                    code = 1
                reason = f"exit code {code}"

            if attempt < max_attempts:
                self.print_log_tail(log_path, lines=40)
                log(
                    f"Startup gate did not pass ({reason}); retrying with capture restart "
                    f"({attempt}/{max_attempts})",
                    color="yellow",
                )
                self._restart_services_for_gate_retry(gate)
                continue

            if gate_timed_out:
                choice = self._prompt_after_gate_failure(gate, reason, max_attempts)
            elif code != 0:
                self.print_log_tail(log_path)
                choice = self._prompt_after_gate_failure(gate, reason, max_attempts)
            else:
                log("Startup gate passed")
                return

            if choice == "retry":
                attempt = 0
                self._restart_services_for_gate_retry(gate)
                continue
            log("Startup gate skipped by user after failed quality check", color="yellow")
            return

    def unregister_running_item(self, item: RunningProcess) -> None:
        item.limiter_stop.set()
        try:
            item.log_file.close()
        except Exception:
            pass
        self.running = [x for x in self.running if x is not item]

    def restart_allowed_for_exit(self, spec: ServiceSpec, code: int) -> Tuple[bool, str]:
        if not spec.restart_on_exit:
            return False, "restart_on_exit=false"
        if spec.restart_on_error_only and code == 0:
            return False, "process exited normally"
        if spec.restart_max_attempts <= 0:
            return False, "restart_max_attempts<=0"
        now = time.monotonic()
        window = max(0.0, spec.restart_window_s)
        history = self.restart_history.setdefault(spec.name, [])
        if window > 0.0:
            history[:] = [t for t in history if now - t <= window]
        if len(history) >= spec.restart_max_attempts:
            return False, f"restart limit reached: {len(history)}/{spec.restart_max_attempts} in {window:.1f}s"
        history.append(now)
        return True, "allowed"

    def handle_restartable_exit(self, item: RunningProcess, code: int) -> bool:
        spec = item.spec
        allowed, reason = self.restart_allowed_for_exit(spec, code)
        if not allowed:
            return False

        log(
            f"{spec.name} exited with code {code}; restarting "
            f"({len(self.restart_history.get(spec.name, []))}/{spec.restart_max_attempts})"
        , color="yellow")
        self.print_log_tail(item.log_path, lines=40)
        self.unregister_running_item(item)

        if spec.restart_backoff_s > 0.0:
            log(f"Waiting {spec.restart_backoff_s:.1f}s before restarting {spec.name}")
            time.sleep(spec.restart_backoff_s)

        if spec.restart_clean_registries:
            self.clean_registry_paths(spec.restart_clean_registries)
        if spec.restart_run_gate:
            self.run_gate(self.cfg.gate)

        started = self.start_service(spec)
        if started and spec.restart_wait_streams:
            self.wait_for_service_streams(spec)
        log(f"{spec.name} restarted successfully", color="green")
        return True

    def run_foreground_service(self, spec: ServiceSpec) -> None:
        if not spec.enabled:
            log(f"Skipping {spec.name}: disabled")
            return
        log(f"Running foreground service {spec.name}")
        log(f"  cmd: {' '.join(spec.command)}")
        if self.dry_run:
            return
        env = os.environ.copy()
        env.update(spec.env)
        code = subprocess.call(spec.command, cwd=spec.cwd or self.cfg.root_project, env=env)
        if code != 0:
            self.fail(f"Foreground service {spec.name} exited with code {code}")

    def run(self, *, once: bool = False) -> None:
        self.log_dir.mkdir(parents=True, exist_ok=True)
        os.chdir(self.cfg.root_project)
        log(f"ROOT_PROJECT={self.cfg.root_project}")
        log(f"LOG_DIR={self.log_dir}")
        log(f"LOG_MAX_BYTES={self.cfg.log_max_bytes}")
        log(f"LOG_TRIM_INTERVAL_SEC={self.cfg.log_trim_interval_s}")
        self.clean_registries()
        self.run_prestart_control()

        for spec in self.cfg.pre_gate_services:
            if not spec.start_on_launch:
                log(f"Skipping {spec.name}: start_on_launch=false")
                continue
            started = self.start_service(spec)
            if started:
                self.wait_for_service_streams(spec)

        self.run_gate(self.cfg.gate)

        self.start_imu_tap_controls()

        for spec in self.cfg.post_gate_services:
            if not spec.start_on_launch:
                log(f"Skipping {spec.name}: start_on_launch=false")
                continue
            started = self.start_service(spec)
            if started:
                self.wait_for_service_streams(spec)

        log("Cascade started successfully")
        log("All systems are ready. You can now use SteamVR/XR.")
        log(f"Logs: {self.log_dir}")

        for spec in self.cfg.foreground_services:
            self.run_foreground_service(spec)

        if not once:
            self.start_manual_control_loop()

        if once:
            log("--once requested; leaving services running and exiting")
            return

        log("Keeping services alive. Press Ctrl+C to stop.")
        while not self.stop_requested:
            self.drain_control_events()
            for item in list(self.running):
                code = item.process.poll()
                if code is not None:
                    if self.handle_restartable_exit(item, int(code)):
                        continue
                    self.print_log_tail(item.log_path)
                    self.fail(f"{item.spec.name} exited with code {code}")
            time.sleep(1.0)

    def stop_all(self) -> None:
        self.stop_requested = True
        if self.tap_detector is not None:
            self.tap_detector.stop()
            self.tap_detector = None
        if self.manual_controls is not None:
            self.manual_controls.stop_requested = True
        terminate_running_processes(self.running, log_fn=log)


def parse_args(argv: Optional[Sequence[str]] = None) -> argparse.Namespace:
    ap = argparse.ArgumentParser(description="Start XR capture/host apps, wait for startup gate, then start backends/runtime.")
    ap.add_argument("--config", help="JSON config; omitted means built-in SHM cascade")
    ap.add_argument("--config-path", help="Directory with JSON configs; shows an interactive menu when more than one config exists")
    ap.add_argument("--root", help="Override root_project")
    ap.add_argument("--print-default-config", action="store_true", help="Print default JSON config and exit")
    ap.add_argument("--dry-run", action="store_true", help="Print actions without launching processes")
    ap.add_argument("--no-gate", action="store_true", help="Skip startup gate")
    ap.add_argument("--prestart-option", help="Override prestart control option id/choice")
    ap.add_argument("--display-mode", choices=["prompt", "60hz", "90hz", "current", "skip"], help="Compatibility alias for --prestart-option")
    ap.add_argument("--gate-debug", action="store_true", help="Print raw startup gate output to the terminal")
    ap.add_argument("--tap-debug", action="store_true", help="Enable IMU tap controls in debug/log mode")
    ap.add_argument("--no-tap-controls", action="store_true", help="Disable IMU tap controls even if config enables them")
    ap.add_argument("--once", action="store_true", help="Start cascade, then exit instead of supervising services")
    return ap.parse_args(argv)


def replace_root_in_spec(spec: ServiceSpec, old_root: str, new_root: str) -> None:
    spec.command = [arg.replace(old_root, new_root) for arg in spec.command]
    if spec.cwd:
        spec.cwd = spec.cwd.replace(old_root, new_root)
    spec.env = {k: v.replace(old_root, new_root) for k, v in spec.env.items()}
    spec.wait_streams = [
        WaitStream(registry=w.registry.replace(old_root, new_root), stream=w.stream, timeout_s=w.timeout_s)
        for w in spec.wait_streams
    ]



def replace_root_in_value(value: Any, old_root: str, new_root: str) -> Any:
    if isinstance(value, str):
        return value.replace(old_root, new_root)
    if isinstance(value, list):
        return [replace_root_in_value(x, old_root, new_root) for x in value]
    if isinstance(value, dict):
        return {k: replace_root_in_value(v, old_root, new_root) for k, v in value.items()}
    return value

def apply_root_override(cfg: ClientConfig, new_root_raw: Optional[str]) -> None:
    if not new_root_raw:
        return
    old_root = cfg.root_project
    new_root = expand_path(new_root_raw)
    cfg.root_project = new_root
    cfg.log_dir = cfg.log_dir.replace(old_root, new_root)
    cfg.registries_to_clean = [x.replace(old_root, new_root) for x in cfg.registries_to_clean]
    for spec in cfg.pre_gate_services + cfg.post_gate_services + cfg.foreground_services:
        replace_root_in_spec(spec, old_root, new_root)
    if cfg.gate is not None:
        cfg.gate.command = [arg.replace(old_root, new_root) for arg in cfg.gate.command]
        if cfg.gate.cwd:
            cfg.gate.cwd = cfg.gate.cwd.replace(old_root, new_root)
        cfg.gate.env = {k: v.replace(old_root, new_root) for k, v in cfg.gate.env.items()}
    if cfg.prestart_control is not None:
        for option in cfg.prestart_control.options:
            option.command = [arg.replace(old_root, new_root) for arg in option.command]
            if option.cwd:
                option.cwd = option.cwd.replace(old_root, new_root)
            option.env = {k: v.replace(old_root, new_root) for k, v in option.env.items()}
    if cfg.imu_tap_controls is not None:
        cfg.imu_tap_controls.source.registry = cfg.imu_tap_controls.source.registry.replace(old_root, new_root)
        cfg.imu_tap_controls.actions = {
            name: replace_root_in_value(action, old_root, new_root)
            for name, action in cfg.imu_tap_controls.actions.items()
        }


def main(argv: Optional[Sequence[str]] = None) -> int:
    args = parse_args(argv)
    if args.print_default_config:
        print(json.dumps(default_config_dict(), indent=2, ensure_ascii=False))
        return 0

    launcher: Optional[Launcher] = None
    try:
        selected_config = resolve_config_argument(args)
        cfg = load_config(selected_config)
        apply_root_override(cfg, args.root)
        if args.no_gate and cfg.gate is not None:
            cfg.gate.enabled = False
        if args.gate_debug and cfg.gate is not None:
            cfg.gate.debug_output = True
        if cfg.prestart_control is not None:
            selected_prestart = args.prestart_option or args.display_mode
            if selected_prestart:
                cfg.prestart_control.selected_option = selected_prestart
                cfg.prestart_control.prompt = str(selected_prestart).lower() in ("prompt", "ask", "interactive")
        if args.no_tap_controls and cfg.imu_tap_controls is not None:
            cfg.imu_tap_controls.enabled = False
        if args.tap_debug:
            if cfg.imu_tap_controls is None:
                cfg.imu_tap_controls = ImuTapControlsSpec(enabled=True, debug=True)
            else:
                cfg.imu_tap_controls.enabled = True
                cfg.imu_tap_controls.debug = True
        launcher = Launcher(cfg, dry_run=args.dry_run)
        launcher.run(once=bool(args.once))
        return 0
    except KeyboardInterrupt:
        print("", file=sys.stderr)
        return 130
    except Exception as exc:
        print(f"[xr_backend_client][ERROR] {exc}", file=sys.stderr, flush=True)
        return 1
    finally:
        if launcher is not None and not args.once:
            launcher.stop_all()


if __name__ == "__main__":
    raise SystemExit(main())
