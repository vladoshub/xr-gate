#!/usr/bin/env python3
"""Shared helpers for xr_client modules."""
from __future__ import annotations

import json
import os
import sys
from pathlib import Path
from typing import Any, Dict, List, Optional, Sequence

ANSI_COLORS = {
    "green": "\033[32m",
    "yellow": "\033[33m",
    "blue": "\033[34m",
    "white": "\033[97m",
    "reset": "\033[0m",
}

IS_WINDOWS = os.name == "nt"
IS_POSIX = os.name == "posix"


def platform_name() -> str:
    return "windows" if IS_WINDOWS else "linux" if IS_POSIX else os.name


def default_scripts_os() -> str:
    """Return the device scripts subdirectory for the current host OS.

    Linux behavior intentionally remains unchanged: scripts still resolve to
    devices/xreal_ultra/linux/scripts unless XR_DEVICE_SCRIPTS_ROOT overrides it.
    """
    raw = os.environ.get("XR_DEVICE_SCRIPTS_OS", "").strip().lower()
    if raw:
        return raw
    return "windows" if IS_WINDOWS else "linux"


def default_temp_dir() -> str:
    if IS_WINDOWS:
        return os.environ.get("XR_TEMP_DIR") or os.environ.get("TEMP") or os.environ.get("TMP") or str(Path.home() / "AppData" / "Local" / "Temp")
    return os.environ.get("XR_TEMP_DIR") or "/tmp"


def default_registry_path(filename: str) -> str:
    return str(Path(default_temp_dir()) / filename)


def default_log_dir() -> str:
    if IS_WINDOWS:
        return os.environ.get("LOG_DIR") or str(Path(default_temp_dir()) / "xr_backend_client_logs")
    return os.environ.get("LOG_DIR") or "/tmp/xr_backend_client_logs"


def default_root_project() -> str:
    return os.environ.get("XR_ROOT_PROJECT", os.environ.get("ROOT_PROJECT", str(Path.home() / "src" / "xr_tracking")))


def powershell_command(script_path: str, *args: str) -> List[str]:
    return [
        os.environ.get("XR_POWERSHELL", "powershell.exe"),
        "-NoProfile",
        "-ExecutionPolicy",
        "Bypass",
        "-File",
        script_path,
        *args,
    ]

def colors_enabled() -> bool:
    raw = os.environ.get("XR_CLIENT_COLOR", "auto").strip().lower()
    if os.environ.get("NO_COLOR") is not None:
        return False
    if raw in ("0", "false", "no", "off", "never"):
        return False
    if raw in ("1", "true", "yes", "on", "always"):
        return True
    return sys.stdout.isatty() and os.environ.get("TERM", "") != "dumb"


def default_log_color(message: str) -> Optional[str]:
    if message.startswith("Startup check:"):
        return "yellow"
    if message == "Startup gate passed":
        return "blue"
    if message == "All systems are ready. You can now use SteamVR/XR.":
        return "white"
    if message == "xr_runtime is running...":
        return "green"
    if message == "Keeping services alive. Press Ctrl+C to stop.":
        return "green"
    return None


def colorize(message: str, color: Optional[str]) -> str:
    if not color or not colors_enabled():
        return message
    prefix = ANSI_COLORS.get(color)
    if not prefix:
        return message
    return f"{prefix}{message}{ANSI_COLORS['reset']}"


def log(message: str, *, color: Optional[str] = None) -> None:
    color = color or default_log_color(message)
    print(f"[xr_backend_client] {colorize(message, color)}", flush=True)


def _path_placeholders(root_project: Optional[str] = None) -> Dict[str, str]:
    root = root_project or os.environ.get("XR_ROOT_PROJECT") or os.environ.get("ROOT_PROJECT") or ""
    device_home = os.environ.get("XR_DEVICE_HOME")
    if not device_home and root:
        device_home = str(Path(root) / "devices" / "xreal_ultra")
    bin_root = os.environ.get("XR_BIN_ROOT")
    if not bin_root and root:
        bin_root = str(Path(root) / "bin")
    scripts_root = os.environ.get("XR_DEVICE_SCRIPTS_ROOT")
    if not scripts_root and device_home:
        scripts_root = str(Path(device_home) / default_scripts_os() / "scripts")
    configs_root = os.environ.get("XR_DEVICE_CONFIGS_ROOT")
    if not configs_root and device_home:
        configs_root = str(Path(device_home) / "configs")
    calib_root = os.environ.get("XR_CALIB_DIR")
    if not calib_root and configs_root:
        calib_root = str(Path(configs_root) / "calibration_dataset")
    return {
        "root": root,
        "python": sys.executable,
        "bin": bin_root or "",
        "device": device_home or "",
        "scripts": scripts_root or "",
        "scripts_os": default_scripts_os(),
        "configs": configs_root or "",
        "calibration": calib_root or "",
        "temp": default_temp_dir(),
    }


def expand_path(value: str, root_project: Optional[str] = None) -> str:
    value = os.path.expanduser(os.path.expandvars(str(value)))
    for key, replacement in _path_placeholders(root_project).items():
        if replacement:
            value = value.replace("{" + key + "}", replacement)
    return value


def expand_command(command: Sequence[str], root_project: str) -> List[str]:
    return [expand_path(part, root_project) for part in command]


def env_truthy(name: str, default: bool) -> bool:
    raw = os.environ.get(name)
    if raw is None:
        return default
    return raw.strip().lower() not in ("0", "false", "no", "off", "")


def service_env_name(name: str) -> str:
    return "RUN_" + "".join(ch if ch.isalnum() else "_" for ch in name.upper())


def deep_update(base: Dict[str, Any], overlay: Dict[str, Any]) -> Dict[str, Any]:
    out = dict(base)
    for key, value in overlay.items():
        if isinstance(value, dict) and isinstance(out.get(key), dict):
            out[key] = deep_update(out[key], value)
        else:
            out[key] = value
    return out


def registry_has_stream(registry: str, stream: str) -> bool:
    path = Path(registry)
    try:
        data = json.loads(path.read_text())
    except Exception:
        return False

    def has_stream(obj: Any) -> bool:
        if isinstance(obj, dict):
            if stream in obj:
                return True
            streams = obj.get("streams")
            if isinstance(streams, dict) and stream in streams:
                return True
            if isinstance(streams, list):
                for item in streams:
                    if isinstance(item, dict):
                        for key in ("stream", "stream_id", "stream_name", "name", "id"):
                            if item.get(key) == stream:
                                return True
            for key in ("stream", "stream_id", "stream_name", "name", "id"):
                if obj.get(key) == stream:
                    return True
            return any(has_stream(v) for v in obj.values())
        if isinstance(obj, list):
            return any(has_stream(v) for v in obj)
        return False

    return has_stream(data)


