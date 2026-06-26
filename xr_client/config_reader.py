#!/usr/bin/env python3
"""Configuration models, defaults and loading for xr_backend_client."""
from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Dict, Iterable, List, Optional, Sequence

try:
    from .common import (
        IS_POSIX,
        IS_WINDOWS,
        deep_update,
        default_log_dir,
        default_registry_path,
        default_root_project,
        default_scripts_os,
        env_truthy,
        expand_command,
        expand_path,
        log,
        powershell_command,
        service_env_name,
    )
    from .tap_controls import ImuTapControlsSpec, parse_imu_tap_controls
except ImportError:
    from common import (
        IS_POSIX,
        IS_WINDOWS,
        deep_update,
        default_log_dir,
        default_registry_path,
        default_root_project,
        default_scripts_os,
        env_truthy,
        expand_command,
        expand_path,
        log,
        powershell_command,
        service_env_name,
    )
    from tap_controls import ImuTapControlsSpec, parse_imu_tap_controls

@dataclass(frozen=True)
class WaitStream:
    registry: str
    stream: str
    timeout_s: float = 30.0


@dataclass
class ServiceSpec:
    name: str
    command: List[str]
    enabled: bool = True
    start_on_launch: bool = True
    optional: bool = False
    cwd: Optional[str] = None
    env: Dict[str, str] = field(default_factory=dict)
    wait_streams: List[WaitStream] = field(default_factory=list)
    foreground: bool = False
    ready_message: str = ""
    start_delay_s: float = 0.3
    stop_timeout_s: float = 1.0
    restart_on_exit: bool = False
    restart_on_error_only: bool = True
    restart_max_attempts: int = 3
    restart_window_s: float = 60.0
    restart_backoff_s: float = 2.0
    restart_wait_streams: bool = True
    restart_run_gate: bool = False
    restart_clean_registries: List[str] = field(default_factory=list)


@dataclass
class GateSpec:
    enabled: bool
    command: List[str]
    cwd: Optional[str] = None
    env: Dict[str, str] = field(default_factory=dict)
    log_name: str = "startup_gate"
    user_hints: bool = True
    debug_output: bool = False
    progress_interval_s: float = 0.5
    timeout_s: float = 30.0
    timeout_status_interval_s: float = 1.0
    timeout_prompt: bool = True
    max_attempts: int = 1
    restart_services_on_failure: List[str] = field(default_factory=list)
    restart_clean_registries: List[str] = field(default_factory=list)
    visual_progress_template: str = (
        "Startup check: {percent:.0f}% — stay in a well-lit area and keep the glasses cameras uncovered."
    )
    visual_ready_template: str = "Startup check: cameras OK. Keep the headset still while IMU stabilizes."
    imu_progress_template: str = "Startup check: cameras OK; headset stability {percent:.0f}% — keep your head still."
    imu_ready_template: str = "Startup check: cameras and headset stability OK."
    quality_progress_template: str = (
        "Startup stream quality: {percent:.0f}% — defective frames {defective}/{max_defective}."
    )
    quality_ready_template: str = "Startup stream quality check passed."


@dataclass
class PrestartOptionSpec:
    id: str
    choice: str
    label: str
    description: str = ""
    aliases: List[str] = field(default_factory=list)
    service_name: Optional[str] = None
    command: List[str] = field(default_factory=list)
    cwd: Optional[str] = None
    env: Dict[str, str] = field(default_factory=dict)
    wait_log_any: List[str] = field(default_factory=list)
    wait_timeout_s: float = 0.0
    readiness_min_alive_s: float = 0.0
    readiness_status_interval_s: float = 1.0
    stop_timeout_s: float = 1.0


@dataclass
class PrestartControlSpec:
    enabled: bool = False
    prompt: bool = True
    title: str = "Prestart control"
    prompt_message: str = "Select startup option:"
    start_message: List[str] = field(default_factory=list)
    selected_option: str = "prompt"
    default_option: str = ""
    pre_capture_wait_s: float = 0.0
    options: List[PrestartOptionSpec] = field(default_factory=list)


@dataclass
class ClientConfig:
    root_project: str
    log_dir: str
    clean_registries: bool
    registries_to_clean: List[str]
    wait_timeout_s: float
    log_max_bytes: int
    log_trim_interval_s: float
    prestart_control: Optional[PrestartControlSpec]
    pre_gate_services: List[ServiceSpec]
    gate: Optional[GateSpec]
    imu_tap_controls: Optional[ImuTapControlsSpec]
    post_gate_services: List[ServiceSpec]
    foreground_services: List[ServiceSpec]


def parse_wait_streams(items: Iterable[Dict[str, Any]], default_timeout_s: float, root_project: str) -> List[WaitStream]:
    result: List[WaitStream] = []
    for item in items or []:
        result.append(
            WaitStream(
                registry=expand_path(str(item["registry"]), root_project),
                stream=str(item["stream"]),
                timeout_s=float(item.get("timeout_s", default_timeout_s)),
            )
        )
    return result


def parse_service(item: Dict[str, Any], root_project: str, default_timeout_s: float) -> ServiceSpec:
    name = str(item["name"])
    default_enabled = bool(item.get("enabled", True))
    enabled = env_truthy(str(item.get("enable_env", service_env_name(name))), default_enabled)
    raw_command = item.get("command") or []
    if isinstance(raw_command, str):
        raw_command = [raw_command]
    command = expand_command([str(x) for x in raw_command], root_project)
    cwd_raw = item.get("cwd")
    cwd = expand_path(str(cwd_raw), root_project) if cwd_raw else root_project
    env = {str(k): expand_path(str(v), root_project) for k, v in dict(item.get("env", {})).items()}
    return ServiceSpec(
        name=name,
        command=command,
        enabled=enabled,
        start_on_launch=bool(item.get("start_on_launch", True)),
        optional=bool(item.get("optional", False)),
        cwd=cwd,
        env=env,
        wait_streams=parse_wait_streams(item.get("wait_streams", []), default_timeout_s, root_project),
        foreground=bool(item.get("foreground", False)),
        ready_message=str(item.get("ready_message", "")),
        start_delay_s=float(item.get("start_delay_s", 0.3)),
        stop_timeout_s=float(item.get("stop_timeout_s", 1.0)),
        restart_on_exit=bool(item.get("restart_on_exit", False)),
        restart_on_error_only=bool(item.get("restart_on_error_only", True)),
        restart_max_attempts=int(item.get("restart_max_attempts", 3)),
        restart_window_s=float(item.get("restart_window_s", 60.0)),
        restart_backoff_s=float(item.get("restart_backoff_s", 2.0)),
        restart_wait_streams=bool(item.get("restart_wait_streams", True)),
        restart_run_gate=bool(item.get("restart_run_gate", False)),
        restart_clean_registries=[str(x) for x in item.get("restart_clean_registries", [])],
    )


def parse_gate(item: Optional[Dict[str, Any]], root_project: str) -> Optional[GateSpec]:
    if not item:
        return None
    enabled = env_truthy(str(item.get("enable_env", "STARTUP_GATE")), bool(item.get("enabled", True)))
    raw_command = item.get("command") or []
    if isinstance(raw_command, str):
        raw_command = [raw_command]
    cwd_raw = item.get("cwd")
    cwd = expand_path(str(cwd_raw), root_project) if cwd_raw else root_project
    env = {str(k): expand_path(str(v), root_project) for k, v in dict(item.get("env", {})).items()}
    return GateSpec(
        enabled=enabled,
        command=expand_command([str(x) for x in raw_command], root_project),
        cwd=cwd,
        env=env,
        log_name=str(item.get("log_name", "startup_gate")),
        user_hints=bool(item.get("user_hints", True)),
        debug_output=bool(item.get("debug_output", False)),
        progress_interval_s=float(item.get("progress_interval_s", 0.5)),
        timeout_s=float(item.get("timeout_s", 30.0)),
        timeout_status_interval_s=float(item.get("timeout_status_interval_s", 1.0)),
        timeout_prompt=bool(item.get("timeout_prompt", True)),
        max_attempts=max(1, int(item.get("max_attempts", 1))),
        restart_services_on_failure=[str(x) for x in item.get("restart_services_on_failure", [])],
        restart_clean_registries=[str(x) for x in item.get("restart_clean_registries", [])],
        visual_progress_template=str(item.get(
            "visual_progress_template",
            "Startup check: {percent:.0f}% — stay in a well-lit area and keep the glasses cameras uncovered.",
        )),
        visual_ready_template=str(item.get(
            "visual_ready_template",
            "Startup check: cameras OK. Keep the headset still while IMU stabilizes.",
        )),
        imu_progress_template=str(item.get(
            "imu_progress_template",
            "Startup check: cameras OK; headset stability {percent:.0f}% — keep your head still.",
        )),
        imu_ready_template=str(item.get(
            "imu_ready_template",
            "Startup check: cameras and headset stability OK.",
        )),
        quality_progress_template=str(item.get(
            "quality_progress_template",
            "Startup stream quality: {percent:.0f}% — defective frames {defective}/{max_defective}.",
        )),
        quality_ready_template=str(item.get(
            "quality_ready_template",
            "Startup stream quality check passed.",
        )),
    )


def _first_option_id(options: Sequence[PrestartOptionSpec]) -> str:
    return options[0].id if options else ""


def parse_prestart_option(item: Dict[str, Any], root_project: str, index: int) -> PrestartOptionSpec:
    option_id = str(item.get("id") or item.get("name") or item.get("choice") or f"option{index + 1}")
    choice = str(item.get("choice") or str(index + 1))
    label = str(item.get("label") or option_id)
    description = str(item.get("description", ""))
    raw_command = item.get("command") or []
    if isinstance(raw_command, str):
        raw_command = [raw_command]
    cwd_raw = item.get("cwd")
    cwd = expand_path(str(cwd_raw), root_project) if cwd_raw else root_project
    env = {str(k): expand_path(str(v), root_project) for k, v in dict(item.get("env", {})).items()}
    service_name_raw = item.get("service_name")
    service_name = str(service_name_raw) if service_name_raw else None
    raw_aliases = item.get("aliases", [])
    if isinstance(raw_aliases, str):
        aliases = [raw_aliases]
    else:
        aliases = [str(x) for x in raw_aliases]
    return PrestartOptionSpec(
        id=option_id,
        choice=choice,
        label=label,
        description=description,
        aliases=aliases,
        service_name=service_name,
        command=expand_command([str(x) for x in raw_command], root_project),
        cwd=cwd,
        env=env,
        wait_log_any=[str(x) for x in item.get("wait_log_any", [])],
        wait_timeout_s=float(item.get("wait_timeout_s", 0.0)),
        readiness_min_alive_s=float(item.get("readiness_min_alive_s", 0.0)),
        readiness_status_interval_s=float(item.get("readiness_status_interval_s", 1.0)),
        stop_timeout_s=float(item.get("stop_timeout_s", 1.0)),
    )


def parse_prestart_control(item: Optional[Dict[str, Any]], root_project: str) -> Optional[PrestartControlSpec]:
    if not item:
        return None
    enabled = env_truthy(str(item.get("enable_env", "RUN_PRESTART_CONTROL")), bool(item.get("enabled", True)))
    options = [parse_prestart_option(x, root_project, idx) for idx, x in enumerate(item.get("options", []))]
    start_message_raw = item.get("start_message", [])
    if isinstance(start_message_raw, str):
        start_message = [start_message_raw]
    else:
        start_message = [str(x) for x in start_message_raw]
    default_option = str(item.get("default_option") or _first_option_id(options))
    selected_option = str(os.environ.get("XR_PRESTART_OPTION", item.get("selected_option", "prompt")))
    return PrestartControlSpec(
        enabled=enabled,
        prompt=bool(item.get("prompt", True)),
        title=str(item.get("title", "Prestart control")),
        prompt_message=str(item.get("prompt_message", "Select startup option:")),
        start_message=start_message,
        selected_option=selected_option,
        default_option=default_option,
        pre_capture_wait_s=float(item.get("pre_capture_wait_s", 0.0)),
        options=options,
    )



def apply_default_runtime_control_actions(config: Dict[str, Any], tracking_registry: str, runtime_registry: str) -> None:
    """Install the built-in manual/tap action policy into a default config dict."""
    post_gate_services = config.setdefault("post_gate_services", [])
    if not any(isinstance(x, dict) and x.get("name") == "imu_3dof_backend" for x in post_gate_services):
        insert_at = 0
        for idx, item in enumerate(post_gate_services):
            if isinstance(item, dict) and item.get("name") == "basalt_vio":
                insert_at = idx + 1
                break
        post_gate_services.insert(insert_at, {
            "name": "imu_3dof_backend",
            "enable_env": "RUN_IMU_3DOF_BACKEND",
            "enabled": True,
            "start_on_launch": False,
            "command": ["{scripts}/imu_3dof/start_imu_3dof_backend.sh"],
            "wait_streams": [{"registry": tracking_registry, "stream": "hmd_pose_3dof"}],
            "restart_on_exit": True,
            "restart_on_error_only": True,
            "restart_max_attempts": 3,
            "restart_window_s": 60,
            "restart_backoff_s": 1.0,
            "restart_wait_streams": True,
            "restart_run_gate": False,
        })

    tap_controls = config.setdefault("imu_tap_controls", {})
    detector = tap_controls.setdefault("detector", {})
    detector.update({
        "double_min_interval_ms": 250.0,
        "double_max_interval_ms": 650.0,
        "double_max_span_ms": 900.0,
        "double_emit_delay_ms": 700.0,
        "triple_min_interval_ms": 250.0,
        "triple_max_interval_ms": 650.0,
        "triple_max_span_ms": 1400.0,
        "triple_emit_delay_ms": 750.0,
        "quadruple_min_interval_ms": 250.0,
        "quadruple_max_interval_ms": 650.0,
        "quadruple_max_span_ms": 2100.0,
    })

    clean_all = [{"registry": tracking_registry, "streams": ["hmd_pose", "hmd_pose_3dof", "hand_tracking"]}]
    clean_hand = [{"registry": tracking_registry, "streams": ["hand_tracking"]}]
    clean_hmd = [{"registry": tracking_registry, "streams": ["hmd_pose"]}]
    clean_hmd3 = [{"registry": tracking_registry, "streams": ["hmd_pose_3dof"]}]
    clean_spatial = [{"registry": runtime_registry, "streams": ["spatial_proxy_mesh", "runtime_spatial_summary"]}]

    restart_running = {
        "type": "restart_services",
        "description": "Restart currently running tracking backends without re-running the startup gate.",
        "services": ["basalt_vio", "imu_3dof_backend", "mercury_hand_tracking", "xr_spatial"],
        "running_only": True,
        "run_gate": False,
        "wait_streams": True,
        "clean_streams": clean_all,
    }
    toggle_hand = {
        "type": "toggle_service",
        "description": "Start or stop Mercury hand tracking.",
        "service": "mercury_hand_tracking",
        "wait_streams": True,
        "clean_streams_on_stop": clean_hand,
        "clean_streams_on_start": clean_hand,
    }
    toggle_3dof = {
        "type": "toggle_exclusive_services",
        "description": "Switch between Basalt 6DoF tracking and IMU-only 3DoF tracking.",
        "primary_service": "basalt_vio",
        "secondary_service": "imu_3dof_backend",
        "start_when_none": "imu_3dof_backend",
        "wait_streams": True,
        "primary_clean_streams_on_stop": clean_hmd,
        "secondary_clean_streams_on_stop": clean_hmd3,
        "primary_clean_streams_on_start": clean_hmd,
        "secondary_clean_streams_on_start": clean_hmd3,
    }
    recenter = {
        "type": "recenter_3dof",
        "description": "Recenter the IMU-only 3DoF yaw origin.",
        "service": "imu_3dof_backend",
        "only_if_service_running": True,
        "control_file": "/tmp/xr_backend_control.json",
        "counter_key": "imu_3dof_recenter_counter",
    }
    toggle_override_controller = {
        "type": "toggle_service",
        "description": "Start or stop the override controller input publisher.",
        "service": "override_controller",
        "wait_streams": True,
        "clean_streams_on_stop": [
            {"registry": tracking_registry, "streams": ["controller_input"]}
        ],
        "clean_streams_on_start": [
            {"registry": tracking_registry, "streams": ["controller_input"]}
        ],
    }
    toggle_xr_video = {
        "type": "toggle_service",
        "description": "Start or stop the xr_video backend.",
        "service": "xr_video",
        "wait_streams": False,
    }
    toggle_xr_spatial = {
        "type": "toggle_service",
        "description": "Start or stop the xr_spatial backend.",
        "service": "xr_spatial",
        "wait_streams": True,
        "clean_streams_on_stop": clean_spatial,
        "clean_streams_on_start": clean_spatial,
    }
    toggle_shake_controls = {
        "type": "toggle_imu_tap_controls",
        "description": "Enable or disable IMU tap controls for this session only; the config file is not changed.",
    }
    tap_controls["actions"] = {
        "left-double-tap": recenter,
        "left-triple-tap": toggle_3dof,
        "left-quadruple-tap": toggle_hand,
        "right-triple-tap": restart_running,
        "right-quadruple-tap": toggle_override_controller,
        "manual-restart-running-backends": restart_running,
        "manual-toggle-hand-tracking": toggle_hand,
        "manual-toggle-3dof-6dof": toggle_3dof,
        "manual-recenter-3dof": recenter,
        "manual-toggle-override-controller": toggle_override_controller,
        "manual-toggle-xr-video": toggle_xr_video,
        "manual-toggle-xr-spatial": toggle_xr_spatial,
        "manual-toggle-shake-controls": toggle_shake_controls,
    }


def _load_key_value_env_file(env_path: str, *, required: bool = False) -> int:
    """Load a simple KEY=VALUE env file without requiring bash.

    This is intentionally conservative and is used for native Windows runs where
    bash is not guaranteed to exist. POSIX keeps the existing bash-compatible
    loader so current Linux xreal_ultra.env behavior is unchanged.
    """
    path = Path(expand_path(env_path))
    if not path.exists():
        if required:
            raise FileNotFoundError(f"device env file not found: {path}")
        return 0
    changed = 0
    for raw_line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        line = raw_line.strip()
        if not line or line.startswith("#"):
            continue
        if line.startswith("export "):
            line = line[len("export "):].strip()
        if "=" not in line:
            continue
        key, value = line.split("=", 1)
        key = key.strip()
        value = value.strip().strip('"').strip("'")
        if not key:
            continue
        value = expand_path(value)
        if os.environ.get(key) != value:
            os.environ[key] = value
            changed += 1
    log(f"Loaded device env: {path} ({changed} changed/new vars)")
    return changed


def _load_shell_env_file(env_path: str, *, required: bool = False) -> int:
    """Load a bash-compatible device env file into this Python process."""
    if not IS_POSIX:
        return _load_key_value_env_file(env_path, required=required)
    path = Path(expand_path(env_path))
    if not path.exists():
        if required:
            raise FileNotFoundError(f"device env file not found: {path}")
        return 0
    before = dict(os.environ)
    cmd = [
        "bash",
        "-lc",
        "set -a; source \"$1\"; env -0",
        "xr_client_env_loader",
        str(path),
    ]
    try:
        proc = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)
    except FileNotFoundError:
        if required:
            raise RuntimeError("bash is required to load device env files")
        return 0
    if proc.returncode != 0:
        raise RuntimeError(
            f"failed to load device env {path}: {proc.stderr.decode(errors='replace').strip()}"
        )
    after: Dict[str, str] = {}
    for raw in proc.stdout.split(b"\0"):
        if not raw or b"=" not in raw:
            continue
        key, value = raw.split(b"=", 1)
        after[key.decode(errors="replace")] = value.decode(errors="replace")
    changed = 0
    for key, value in after.items():
        if before.get(key) != value:
            os.environ[key] = value
            changed += 1
    log(f"Loaded device env: {path} ({changed} changed/new vars)")
    return changed


def _load_config_device_env(data: Dict[str, Any], initial_root_project: str) -> str:
    raw = os.environ.get("XR_DEVICE_ENV") or data.get("device_env")
    if raw is None:
        raw = str(Path(initial_root_project) / "devices" / "xreal_ultra" / "xreal_ultra.env")
    env_path = expand_path(str(raw), initial_root_project)
    os.environ.setdefault("XR_DEVICE_ENV", env_path)
    if "device_env_required" in data:
        required = bool(data.get("device_env_required"))
    else:
        required = bool(data.get("device_env"))
    _load_shell_env_file(env_path, required=required)
    root_project = expand_path(str(os.environ.get("XR_ROOT_PROJECT", initial_root_project)))
    os.environ.setdefault("XR_ROOT_PROJECT", root_project)
    os.environ.setdefault("ROOT_PROJECT", root_project)
    os.environ.setdefault("XR_BIN_ROOT", str(Path(root_project) / "bin"))
    os.environ.setdefault("XR_DEVICE_HOME", str(Path(root_project) / "devices" / "xreal_ultra"))
    os.environ.setdefault("XR_DEVICE_SCRIPTS_OS", default_scripts_os())
    os.environ.setdefault("XR_DEVICE_SCRIPTS_ROOT", str(Path(os.environ["XR_DEVICE_HOME"]) / os.environ["XR_DEVICE_SCRIPTS_OS"] / "scripts"))
    os.environ.setdefault("XR_DEVICE_CONFIGS_ROOT", str(Path(os.environ["XR_DEVICE_HOME"]) / "configs"))
    return root_project

def default_linux_config_dict() -> Dict[str, Any]:
    root = os.environ.get("XR_ROOT_PROJECT", os.environ.get("ROOT_PROJECT", "~/src/xr_tracking"))
    capture_registry = os.environ.get("CAPTURE_REGISTRY", "/tmp/capture_service_streams.json")
    tracking_registry = os.environ.get("TRACKING_REGISTRY", "/tmp/tracking_streams.json")
    runtime_registry = os.environ.get("RUNTIME_REGISTRY", "/tmp/runtime_tracking_streams.json")
    config = {
        "root_project": root,
        "device_env": os.environ.get("XR_DEVICE_ENV", "{root}/devices/xreal_ultra/xreal_ultra.env"),
        "log_dir": os.environ.get("LOG_DIR", "/tmp/xr_backend_client_logs"),
        "clean_registries": env_truthy("CLEAN_REGISTRIES", True),
        "registries_to_clean": [capture_registry, tracking_registry, runtime_registry],
        "wait_timeout_s": float(os.environ.get("WAIT_TIMEOUT_SEC", "30")),
        "log_max_bytes": int(os.environ.get("LOG_MAX_BYTES", "1048576")),
        "log_trim_interval_s": float(os.environ.get("LOG_TRIM_INTERVAL_SEC", "2")),
        "pre_gate_services": [
            {
                "name": "capture_service",
                "enable_env": "RUN_CAPTURE_SERVICE",
                "enabled": True,
                "command": ["{scripts}/capture_service/start_capture_service.sh"],
                "wait_streams": [
                    {"registry": capture_registry, "stream": "camera0"},
                    {"registry": capture_registry, "stream": "camera1"},
                    {"registry": capture_registry, "stream": "imu0"},
                ],
            },
            {
                "name": "override_controller_service",
                "enable_env": "RUN_OVERRIDE_CONTROLLER_SERVICE",
                "enabled": False,
                "optional": True,
                "command": ["{root}/bin/host/override_controller_service"],
            },
        ],
        "gate": {
            "enabled": True,
            "enable_env": "STARTUP_GATE",
            "log_name": "startup_gate",
            "user_hints": True,
            "debug_output": False,
            "progress_interval_s": 0.5,
            "timeout_s": 12.0,
            "timeout_status_interval_s": 1.0,
            "timeout_prompt": True,
            "max_attempts": 5,
            "restart_services_on_failure": ["capture_service"],
            "restart_clean_registries": [capture_registry],
            "quality_progress_template": "Startup stream quality: {percent:.0f}% — defective frames {defective}/{max_defective}.",
            "quality_ready_template": "Startup stream quality check passed.",
            "command": [
                "{python}",
                "{root}/xr_client/tools/xr_startup_gate.py",
                "--transport", "shm",
                "--registry", capture_registry,
                "--cam0-stream", "camera0",
                "--cam1-stream", "camera1",
                "--imu-stream", "imu0",
                "--stream-quality-gate",
                "--quality-window-s", "5.0",
                "--quality-max-defective-frames", "5",
                "--quality-min-frames", "20",
                "--quality-min-mean", "12.0",
                "--quality-min-stddev", "4.0",
                "--quality-max-black-fraction", "0.90",
                "--quality-min-corners", "4",
                "--quality-min-laplacian-stddev", "3.0",
                "--print-every", "10",
            ],
        },
        "imu_tap_controls": {
            "enabled": True,
            "enable_env": "RUN_IMU_TAP_CONTROLS",
            "debug": False,
            "source": {
                "transport": "shm",
                "registry": capture_registry,
                "imu_stream": "imu0",
                "poll_sleep_ms": 1.0,
            },
            "detector": {
                "tap_accel_threshold": 15.0,
                "tap_refractory_ms": 100.0,
                "impact_end_ratio": 0.70,
                "impact_max_width_ms": 60.0,
                "triple_min_interval_ms": 250.0,
                "triple_max_interval_ms": 650.0,
                "triple_max_span_ms": 1400.0,
                "cooldown_ms": 4000.0,
                "side_axis": "ax",
                "side_deadzone_mps2": 3.0,
            },
            "actions": {
                "left-triple-tap": {
                    "type": "none",
                    "description": "reserved/no-op for now",
                },
                "right-triple-tap": {
                    "type": "restart_services",
                    "services": ["basalt_vio", "mercury_hand_tracking"],
                    "run_gate": False,
                    "clean_registries": [tracking_registry],
                },
            },
        },
        "post_gate_services": [
            {
                "name": "basalt_vio",
                "enable_env": "RUN_BASALT",
                "enabled": True,
                "command": ["{scripts}/basalt_vio/start_basalt.sh"],
                "wait_streams": [{"registry": tracking_registry, "stream": "hmd_pose"}],
            },
            {
                "name": "mercury_hand_tracking",
                "enable_env": "RUN_HAND_TRACKING",
                "enabled": True,
                "command": ["{scripts}/mercury_hand_tracking/start_hand_tracking.sh"],
                "wait_streams": [{"registry": tracking_registry, "stream": "hand_tracking"}],
            },
            {
                "name": "xr_runtime_adapter",
                "enable_env": "RUN_XR_RUNTIME_ADAPTER",
                "enabled": True,
                "command": ["{scripts}/xr_runtime_adapter/start_xr_runtime_adapter_shm.sh"],
                "env": {
                    "HMD_3DOF_PRIORITY": "1",
                    "HMD_3DOF_STREAM": "hmd_pose_3dof",
                    "HMD_3DOF_REGISTRY": tracking_registry,
                },
                "wait_streams": [
                    {"registry": runtime_registry, "stream": "runtime_hmd_pose"},
                    {"registry": runtime_registry, "stream": "runtime_hand_tracking"},
                ],
                "ready_message": "xr_runtime is running...",
            },
            {
                "name": "override_controller",
                "enable_env": "RUN_OVERRIDE_CONTROLLER",
                "enabled": True,
                "start_on_launch": False,
                "optional": True,
                "command": ["{scripts}/override_controller/start_override_controller.sh"],
                "env": {
                    "CONFIG_PATH": "~/.config/xr_tracking/override_controller/default.json",
                    "NON_INTERACTIVE": "1",
                    "GRAB_DEVICES": "1",
                    "REATTACH_DEVICES": "1",
                    "REATTACH_INTERVAL_MS": "3000",
                },
                "wait_streams": [
                    {"registry": tracking_registry, "stream": "controller_input"},
                ],
                "ready_message": "override_controller is running. Right quadruple tap or manual key 5 toggles it.",
                "start_delay_s": 0.2,
                "stop_timeout_s": 1.0,
                "restart_on_exit": True,
                "restart_on_error_only": True,
                "restart_max_attempts": 3,
                "restart_window_s": 60,
                "restart_backoff_s": 1.0,
                "restart_wait_streams": True,
                "restart_run_gate": False,
            },
            {
                "name": "xr_video",
                "enable_env": "RUN_XR_VIDEO",
                "enabled": True,
                "start_on_launch": False,
                "optional": True,
                "command": ["{scripts}/xr_video/start_xr_video_backend.sh"],
                "ready_message": "xr_video is running. Manual key 6 toggles it.",
                "start_delay_s": 0.2,
                "stop_timeout_s": 1.0,
                "restart_on_exit": True,
                "restart_on_error_only": True,
                "restart_max_attempts": 3,
                "restart_window_s": 60,
                "restart_backoff_s": 1.0,
                "restart_wait_streams": False,
                "restart_run_gate": False,
            },
            {
                "name": "xr_spatial",
                "enable_env": "RUN_XR_SPATIAL",
                "enabled": True,
                "start_on_launch": False,
                "optional": True,
                "command": ["{scripts}/xr_spatial/start_xr_spatial_shm.sh"],
                "env": {
                    "SPATIAL_PROXY_MESH_RATE_HZ": "10",
                },
                "wait_streams": [
                    {"registry": runtime_registry, "stream": "spatial_proxy_mesh"},
                ],
                "ready_message": "xr_spatial is running. Manual key 7 toggles it.",
                "start_delay_s": 0.2,
                "stop_timeout_s": 1.0,
                "restart_on_exit": True,
                "restart_on_error_only": True,
                "restart_max_attempts": 3,
                "restart_window_s": 60,
                "restart_backoff_s": 1.0,
                "restart_wait_streams": True,
                "restart_run_gate": False,
            },
        ],
        "foreground_services": [
            {
                "name": "runtime_debug_viewer",
                "enable_env": "RUN_VIEWER",
                "enabled": False,
                "foreground": True,
                "command": [
                    "{python}",
                    "{root}/tools/runtime_debug_viewer/xr_runtime_debug_viewer.py",
                    "--config",
                    "{configs}/runtime_debug_viewer/xr_runtime_stock.yaml",
                ],
            }
        ],
    }

    apply_default_runtime_control_actions(config, tracking_registry, runtime_registry)
    return config


def default_windows_config_dict() -> Dict[str, Any]:
    """Native Windows defaults.

    This is intentionally conservative: Windows starts the components that now
    have native TCP/UDP paths and leaves Linux-only SHM/backends disabled.
    Linux still uses default_linux_config_dict() byte-for-byte in behavior.
    """
    root = default_root_project()
    capture_registry = os.environ.get("CAPTURE_REGISTRY", default_registry_path("capture_service_streams.json"))
    tracking_registry = os.environ.get("TRACKING_REGISTRY", default_registry_path("tracking_streams.json"))
    runtime_registry = os.environ.get("RUNTIME_REGISTRY", default_registry_path("runtime_tracking_streams.json"))
    tcp_host = os.environ.get("CAPTURE_TCP_HOST", "127.0.0.1")
    tcp_port = os.environ.get("CAPTURE_TCP_PORT", "45660")
    config: Dict[str, Any] = {
        "root_project": root,
        "device_env": os.environ.get("XR_DEVICE_ENV", "{root}/devices/xreal_ultra/xreal_ultra.windows.env"),
        "device_env_required": False,
        "log_dir": default_log_dir(),
        "clean_registries": False,
        "registries_to_clean": [capture_registry, tracking_registry, runtime_registry],
        "wait_timeout_s": float(os.environ.get("WAIT_TIMEOUT_SEC", "30")),
        "log_max_bytes": int(os.environ.get("LOG_MAX_BYTES", "1048576")),
        "log_trim_interval_s": float(os.environ.get("LOG_TRIM_INTERVAL_SEC", "2")),
        "prestart_control": {
            "enabled": env_truthy("RUN_PRESTART_CONTROL", False),
            "prompt": True,
            "title": "Prestart control",
            "prompt_message": "Select XREAL display mode:",
            "default_option": "90hz",
            "options": [
                {
                    "id": "60hz",
                    "choice": "1",
                    "label": "60Hz high-refresh SBS/3D - start display helper",
                    "service_name": "xreal_display_helper",
                    "command": powershell_command(
                        "{root}/xreal_display_helper/scripts/windows/start_xreal_display_helper.ps1",
                        "-Root", "{root}",
                        "-Mode", "60hz",
                        "-KeepRunning",
                    ),
                    "readiness_min_alive_s": 1.0,
                    "stop_timeout_s": 1.0,
                },
                {
                    "id": "90hz",
                    "choice": "2",
                    "label": "90Hz high-refresh SBS/3D - start display helper",
                    "service_name": "xreal_display_helper",
                    "command": powershell_command(
                        "{root}/xreal_display_helper/scripts/windows/start_xreal_display_helper.ps1",
                        "-Root", "{root}",
                        "-Mode", "90hz",
                        "-KeepRunning",
                    ),
                    "readiness_min_alive_s": 1.0,
                    "stop_timeout_s": 1.0,
                },
                {
                    "id": "current",
                    "choice": "3",
                    "label": "Use current display mode",
                    "description": "skip display helper",
                    "aliases": ["skip", "none", "current-display"],
                    "command": [],
                },
            ],
        },
        "pre_gate_services": [
            {
                "name": "capture_service",
                "enable_env": "RUN_CAPTURE_SERVICE",
                "enabled": True,
                "command": powershell_command(
                    "{scripts}/capture_service/start_capture_service_cpp.ps1",
                    "-Root", "{root}",
                    "-Publish", "tcp",
                    "-TcpBindHost", "127.0.0.1",
                    "-TcpPort", tcp_port,
                ),
                "start_delay_s": 1.0,
                "stop_timeout_s": 1.0,
            },
        ],
        "gate": {
            "enabled": env_truthy("STARTUP_GATE", True),
            "enable_env": "STARTUP_GATE",
            "log_name": "startup_gate",
            "user_hints": True,
            "debug_output": False,
            "progress_interval_s": 0.5,
            "timeout_s": 12.0,
            "timeout_status_interval_s": 1.0,
            "timeout_prompt": True,
            "max_attempts": 5,
            "restart_services_on_failure": ["capture_service"],
            "restart_clean_registries": [],
            "quality_progress_template": "Startup stream quality: {percent:.0f}% — defective frames {defective}/{max_defective}.",
            "quality_ready_template": "Startup stream quality check passed.",
            "command": [
                "{python}",
                "{root}/xr_client/tools/xr_startup_gate.py",
                "--transport", "tcp",
                "--tcp-host", tcp_host,
                "--tcp-port", tcp_port,
                "--cam0-stream", "camera0",
                "--cam1-stream", "camera1",
                "--imu-stream", "imu0",
                "--stream-quality-gate",
                "--quality-window-s", "5.0",
                "--quality-max-defective-frames", "5",
                "--quality-min-frames", "20",
                "--quality-min-mean", "12.0",
                "--quality-min-stddev", "4.0",
                "--quality-max-black-fraction", "0.90",
                "--quality-min-corners", "4",
                "--quality-min-laplacian-stddev", "3.0",
                "--print-every", "10",
            ],
        },
        "imu_tap_controls": {
            "enabled": env_truthy("RUN_IMU_TAP_CONTROLS", False),
            "enable_env": "RUN_IMU_TAP_CONTROLS",
            "debug": False,
            "source": {
                "transport": "tcp",
                "tcp_host": tcp_host,
                "tcp_port": int(tcp_port),
                "imu_stream": "imu0",
                "poll_sleep_ms": 1.0,
            },
            "detector": {
                "tap_accel_threshold": 15.0,
                "tap_refractory_ms": 100.0,
                "impact_end_ratio": 0.70,
                "impact_max_width_ms": 60.0,
                "triple_min_interval_ms": 250.0,
                "triple_max_interval_ms": 650.0,
                "triple_max_span_ms": 1400.0,
                "cooldown_ms": 4000.0,
                "side_axis": "ax",
                "side_deadzone_mps2": 3.0,
            },
            "actions": {},
        },
        "post_gate_services": [
            {
                "name": "xr_runtime_adapter",
                "enable_env": "RUN_XR_RUNTIME_ADAPTER",
                "enabled": True,
                "command": powershell_command(
                    "{scripts}/xr_runtime_adapter/start_xr_runtime_adapter_udp.ps1",
                    "-Root", "{root}",
                    "-WithRuntimeHandUdp",
                    "-WithRuntimeControllerStateUdp",
                    "-WithControllerInputTcp",
                ),
                "start_delay_s": 1.0,
                "stop_timeout_s": 1.0,
                "ready_message": "xr_runtime is running...",
            },
            {
                "name": "override_controller",
                "enable_env": "RUN_OVERRIDE_CONTROLLER",
                "enabled": True,
                "start_on_launch": False,
                "optional": True,
                "command": powershell_command(
                    "{scripts}/override_controller/start_override_controller_tcp.ps1",
                    "-Root", "{root}",
                ),
                "start_delay_s": 0.2,
                "stop_timeout_s": 1.0,
                "ready_message": "override_controller is running. Manual key 5 toggles it.",
            },
        ],
        "foreground_services": [],
    }
    apply_default_runtime_control_actions(config, tracking_registry, runtime_registry)
    return config


def default_config_dict() -> Dict[str, Any]:
    if IS_WINDOWS:
        return default_windows_config_dict()
    return default_linux_config_dict()


def wait_for_enter(message: str = "Press Enter to exit.") -> None:
    if not sys.stdin.isatty():
        print(message, flush=True)
        return
    try:
        input(message)
    except EOFError:
        pass


def find_config_files(config_path: str) -> List[Path]:
    path = Path(expand_path(config_path))
    if not path.exists():
        print(f"[xr_backend_client][ERROR] Config directory does not exist: {path}", file=sys.stderr, flush=True)
        wait_for_enter()
        raise SystemExit(2)
    if not path.is_dir():
        print(f"[xr_backend_client][ERROR] --config-path must point to a directory: {path}", file=sys.stderr, flush=True)
        wait_for_enter()
        raise SystemExit(2)
    return sorted([p for p in path.iterdir() if p.is_file() and p.suffix.lower() == ".json"], key=lambda x: x.name.lower())


def read_config_title(path: Path) -> str:
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except Exception:
        return ""
    for key in ("title", "name", "description"):
        value = data.get(key) if isinstance(data, dict) else None
        if isinstance(value, str) and value.strip():
            return value.strip()
    return ""


def config_menu_label(path: Path) -> str:
    title = read_config_title(path)
    return f"{path.name} — {title}" if title else path.name


def read_single_key() -> str:
    if IS_WINDOWS:
        try:
            import msvcrt  # type: ignore
            ch = msvcrt.getwch()
            if ch in ("\x00", "\xe0"):
                ch2 = msvcrt.getwch()
                if ch2 == "H":
                    return "up"
                if ch2 == "P":
                    return "down"
                return ""
            if ch in ("\r", "\n"):
                return "enter"
            if ch in ("q", "Q", "\x1b"):
                return "quit"
            return ch
        except Exception:
            return ""

    try:
        import termios
        import tty
        fd = sys.stdin.fileno()
        old_settings = termios.tcgetattr(fd)
        try:
            tty.setraw(fd)
            ch = sys.stdin.read(1)
            if ch == "\x1b":
                ch2 = sys.stdin.read(1)
                ch3 = sys.stdin.read(1)
                if ch2 == "[" and ch3 == "A":
                    return "up"
                if ch2 == "[" and ch3 == "B":
                    return "down"
                return "quit"
            if ch in ("\r", "\n"):
                return "enter"
            if ch in ("q", "Q"):
                return "quit"
            return ch
        finally:
            termios.tcsetattr(fd, termios.TCSADRAIN, old_settings)
    except Exception:
        return ""


def choose_config_with_number_menu(configs: Sequence[Path]) -> Path:
    print("Select XR client config:", flush=True)
    for idx, path in enumerate(configs, start=1):
        print(f"  {idx} - {config_menu_label(path)}", flush=True)
    while True:
        raw = input("Enter choice [1]: ").strip() or "1"
        try:
            idx = int(raw)
        except ValueError:
            print("Please enter a number.", flush=True)
            continue
        if 1 <= idx <= len(configs):
            return configs[idx - 1]
        print(f"Please enter a number between 1 and {len(configs)}.", flush=True)


def render_config_arrow_menu(configs: Sequence[Path], selected: int, *, redraw: bool) -> None:
    line_count = len(configs) + 3
    if redraw:
        sys.stdout.write(f"\x1b[{line_count}F")
    print("Select XR client config:")
    print("Use ↑/↓ and Enter. Press q to cancel.")
    print("")
    for idx, path in enumerate(configs):
        marker = "➜" if idx == selected else " "
        print(f"  {marker} {config_menu_label(path)}\x1b[K")
    sys.stdout.flush()


def choose_config_with_arrow_menu(configs: Sequence[Path]) -> Path:
    if not sys.stdin.isatty() or not sys.stdout.isatty():
        return choose_config_with_number_menu(configs)

    selected = 0
    render_config_arrow_menu(configs, selected, redraw=False)
    while True:
        key = read_single_key()
        if key == "up":
            selected = (selected - 1) % len(configs)
            render_config_arrow_menu(configs, selected, redraw=True)
        elif key == "down":
            selected = (selected + 1) % len(configs)
            render_config_arrow_menu(configs, selected, redraw=True)
        elif key == "enter":
            print("")
            return configs[selected]
        elif key and key.isdigit():
            idx = int(key) - 1
            if 0 <= idx < len(configs):
                selected = idx
                render_config_arrow_menu(configs, selected, redraw=True)
        elif key == "quit":
            print("\nConfig selection cancelled.", flush=True)
            raise SystemExit(130)


def resolve_config_argument(args: argparse.Namespace) -> Optional[str]:
    if args.config and args.config_path:
        print("[xr_backend_client][ERROR] Use either --config or --config-path, not both.", file=sys.stderr, flush=True)
        wait_for_enter()
        raise SystemExit(2)
    if not args.config_path:
        return args.config

    configs = find_config_files(args.config_path)
    if not configs:
        config_dir = Path(expand_path(args.config_path))
        print(
            f"[xr_backend_client][ERROR] No JSON configs found in {config_dir}. At least one config is required.",
            file=sys.stderr,
            flush=True,
        )
        wait_for_enter()
        raise SystemExit(2)

    if len(configs) == 1:
        selected = configs[0]
        log(f"Using the only available config: {selected}")
        return str(selected)

    selected = choose_config_with_arrow_menu(configs)
    log(f"Selected config: {selected}")
    return str(selected)

def load_config(path: Optional[str]) -> ClientConfig:
    data = default_config_dict()
    if path:
        with open(expand_path(path), "r", encoding="utf-8") as f:
            overlay = json.load(f)
        data = deep_update(data, overlay)

    initial_root_project = expand_path(str(os.environ.get("XR_ROOT_PROJECT") or data.get("root_project", "~/src/xr_tracking")))
    root_project = _load_config_device_env(data, initial_root_project)
    log_dir = expand_path(str(data.get("log_dir", "/tmp/xr_backend_client_logs")), root_project)
    wait_timeout_s = float(data.get("wait_timeout_s", 30.0))

    return ClientConfig(
        root_project=root_project,
        log_dir=log_dir,
        clean_registries=bool(data.get("clean_registries", True)),
        registries_to_clean=[expand_path(str(x), root_project) for x in data.get("registries_to_clean", [])],
        wait_timeout_s=wait_timeout_s,
        log_max_bytes=int(data.get("log_max_bytes", 1048576)),
        log_trim_interval_s=float(data.get("log_trim_interval_s", 2.0)),
        prestart_control=parse_prestart_control(data.get("prestart_control"), root_project),
        pre_gate_services=[parse_service(x, root_project, wait_timeout_s) for x in data.get("pre_gate_services", [])],
        gate=parse_gate(data.get("gate"), root_project),
        imu_tap_controls=parse_imu_tap_controls(data.get("imu_tap_controls"), root_project),
        post_gate_services=[parse_service(x, root_project, wait_timeout_s) for x in data.get("post_gate_services", [])],
        foreground_services=[parse_service(x, root_project, wait_timeout_s) for x in data.get("foreground_services", [])],
    )



