#!/usr/bin/env python3
"""Bounded child-process shutdown helpers for xr_backend_client.

The shutdown path is deliberately cross-platform:
- POSIX: each service is started in a new session, so we can SIGTERM/SIGKILL
  the whole process group.
- Windows: each service is started in a new process group. We first try a
  console CTRL_BREAK_EVENT for graceful shutdown, then fall back to taskkill
  /T /F so child processes are not left behind.
"""
from __future__ import annotations

import os
import signal
import subprocess
import threading
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Callable, List

try:
    from .config_reader import ServiceSpec
except ImportError:
    from config_reader import ServiceSpec

IS_POSIX = os.name == "posix"
IS_WINDOWS = os.name == "nt"


@dataclass
class RunningProcess:
    spec: ServiceSpec
    process: subprocess.Popen
    log_path: Path
    log_file: Any
    limiter_stop: threading.Event = field(default_factory=threading.Event)


def _set_limiter_stop(item: RunningProcess) -> None:
    try:
        if item.limiter_stop is not None:
            item.limiter_stop.set()
    except Exception:
        pass


def _windows_taskkill_tree(proc: subprocess.Popen, *, force: bool) -> bool:
    """Terminate a native Windows process tree with taskkill.

    Returns True when taskkill was executed successfully enough that the caller
    can re-check proc.poll(). False means the fallback was unavailable/failed.
    """
    if not IS_WINDOWS or proc.poll() is not None:
        return False
    command = ["taskkill", "/PID", str(proc.pid), "/T"]
    if force:
        command.append("/F")
    try:
        subprocess.run(
            command,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            timeout=2.0,
            check=False,
        )
        return True
    except Exception:
        return False


def _terminate_process_group(proc: subprocess.Popen) -> None:
    if proc.poll() is not None:
        return
    try:
        if IS_POSIX:
            os.killpg(proc.pid, signal.SIGTERM)
            return
        if IS_WINDOWS:
            # Works for console subprocesses created with CREATE_NEW_PROCESS_GROUP.
            # Non-console processes may ignore it or reject it; then we fall back
            # to terminate(), and the later kill stage uses taskkill /T /F.
            try:
                proc.send_signal(signal.CTRL_BREAK_EVENT)  # type: ignore[attr-defined]
                return
            except Exception:
                pass
            try:
                proc.terminate()
                return
            except Exception:
                pass
        proc.terminate()
    except Exception:
        try:
            proc.terminate()
        except Exception:
            pass


def _kill_process_group(proc: subprocess.Popen) -> None:
    if proc.poll() is not None:
        return
    try:
        if IS_POSIX:
            os.killpg(proc.pid, signal.SIGKILL)
            return
        if IS_WINDOWS:
            if _windows_taskkill_tree(proc, force=True):
                return
            try:
                proc.kill()
                return
            except Exception:
                pass
        proc.kill()
    except Exception:
        try:
            proc.kill()
        except Exception:
            pass


def terminate_one_process_tree(
    proc: subprocess.Popen,
    *,
    graceful_wait_s: float = 0.75,
    kill_wait_s: float = 1.0,
) -> None:
    """Best-effort bounded shutdown for a standalone helper process."""
    if proc.poll() is not None:
        return
    _terminate_process_group(proc)
    try:
        proc.wait(timeout=max(0.0, graceful_wait_s))
        return
    except Exception:
        pass
    if proc.poll() is None:
        _kill_process_group(proc)
    try:
        proc.wait(timeout=max(0.0, kill_wait_s))
    except Exception:
        pass


def _wait_until_exited(items: List[RunningProcess], deadline_s: float) -> bool:
    while time.monotonic() < deadline_s:
        if all(item.process.poll() is not None for item in items):
            return True
        time.sleep(0.05)
    return all(item.process.poll() is not None for item in items)


def terminate_running_processes(
    running: List[RunningProcess],
    *,
    log_fn: Callable[[str], None],
    kill_wait_s: float = 2.0,
) -> None:
    """Stop children without allowing supervisor shutdown to hang forever.

    The caller owns the `running` list. This function always clears it after
    closing log files, even when a child had to be SIGKILLed/taskkill'ed.
    """
    for item in list(running):
        _set_limiter_stop(item)

    if not running:
        return

    log_fn("Stopping child processes...")

    items = list(running)
    for item in reversed(items):
        _terminate_process_group(item.process)

    stop_timeout_s = max((float(item.spec.stop_timeout_s) for item in items), default=1.0)
    stop_timeout_s = max(0.2, stop_timeout_s)
    _wait_until_exited(items, time.monotonic() + stop_timeout_s)

    alive = [item for item in items if item.process.poll() is None]
    if alive:
        log_fn(
            "Force killing unresponsive child processes: "
            + ", ".join(item.spec.name for item in alive)
        )
        for item in reversed(alive):
            _kill_process_group(item.process)
        _wait_until_exited(alive, time.monotonic() + max(0.2, kill_wait_s))

    still_alive = [item for item in items if item.process.poll() is None]
    if still_alive:
        log_fn(
            "Warning: some child processes did not report exit after kill: "
            + ", ".join(item.spec.name for item in still_alive)
        )

    for item in reversed(items):
        try:
            item.process.wait(timeout=0.05)
        except Exception:
            pass
        try:
            item.log_file.close()
        except Exception:
            pass

    running.clear()
