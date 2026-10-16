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
import re
import signal
import subprocess
import sys
import threading
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Callable, List, Optional

try:
    from .config_reader import ServiceSpec
except ImportError:
    from config_reader import ServiceSpec

IS_POSIX = os.name == "posix"
IS_WINDOWS = os.name == "nt"
BLUEZ_DEVICE_PATH = re.compile(r"/org/bluez/hci[0-9]+/dev_(?:[0-9A-Fa-f]{2}_){5}[0-9A-Fa-f]{2}")


@dataclass
class RunningProcess:
    spec: ServiceSpec
    process: subprocess.Popen
    log_path: Path
    log_file: Any
    limiter_stop: threading.Event = field(default_factory=threading.Event)
    ble_cleanup_file: Optional[Path] = None


def cleanup_ble_connections(item: RunningProcess, *, log_fn: Callable[[str], None]) -> None:
    """Recover links left behind by a crashed/killed Linux Gear VR provider."""
    path = item.ble_cleanup_file
    if path is None or item.process.poll() is None:
        return  # Never disconnect a live provider that could reconnect immediately.
    try:
        pending = set()
        for record in path.read_text().splitlines():
            if len(record) < 2 or BLUEZ_DEVICE_PATH.fullmatch(record[1:]) is None:
                continue
            if record[0] == "+":
                pending.add(record[1:])
            elif record[0] == "-":
                pending.discard(record[1:])
        deadline = time.monotonic() + 3.0
        for device_path in sorted(pending):
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                log_fn("[WARN] Gear VR exit cleanup timed out")
                break
            try:
                result = subprocess.run(
                    ["busctl", "--system", "--timeout=1s", "call", "org.bluez",
                     device_path, "org.bluez.Device1", "Disconnect"],
                    stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                    stderr=subprocess.PIPE, text=True, timeout=min(1.5, remaining),
                    check=False,
                )
                if result.returncode == 0 or "Not connected" in result.stderr or "NotConnected" in result.stderr:
                    log_fn(f"Gear VR disconnected after override_controller exit: {device_path}")
                else:
                    log_fn(f"[WARN] Gear VR exit cleanup failed for {device_path}: {result.stderr.strip()}")
            except (OSError, subprocess.TimeoutExpired) as exc:
                log_fn(f"[WARN] Cannot disconnect Gear VR after override_controller exit: {exc}")
    except FileNotFoundError:
        pass
    except (OSError, UnicodeError) as exc:
        log_fn(f"[WARN] Cannot read Gear VR exit cleanup file: {exc}")
    finally:
        try:
            path.unlink(missing_ok=True)
        except OSError as exc:
            log_fn(f"[WARN] Cannot remove Gear VR exit cleanup file: {exc}")
        item.ble_cleanup_file = None


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
    if not IS_POSIX and proc.poll() is not None:
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
    if not IS_POSIX and proc.poll() is not None:
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


def _process_tree_alive(proc: subprocess.Popen) -> bool:
    if proc.poll() is None:
        return True
    if not IS_POSIX:
        return False
    try:
        os.killpg(proc.pid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        return True
    if sys.platform.startswith("linux"):
        # A wrapper may have exited while its children still own devices.
        # Orphan zombies cannot own devices and may await the container's init.
        try:
            for entry in Path("/proc").iterdir():
                if not entry.name.isdigit():
                    continue
                try:
                    fields = (entry / "stat").read_text().rsplit(")", 1)[1].split()
                except (FileNotFoundError, ProcessLookupError):
                    continue
                if int(fields[2]) == proc.pid and fields[0] not in ("Z", "X"):
                    return True
            return False
        except (OSError, ValueError, IndexError):
            pass  # Retain the process-group check if /proc cannot be inspected.
    return True


def _wait_until_exited(processes: List[subprocess.Popen], deadline_s: float) -> bool:
    while time.monotonic() < deadline_s:
        if all(not _process_tree_alive(proc) for proc in processes):
            return True
        time.sleep(0.05)
    return all(not _process_tree_alive(proc) for proc in processes)


def terminate_one_process_tree(
    proc: subprocess.Popen,
    *,
    graceful_wait_s: float = 0.75,
    kill_wait_s: float = 1.0,
) -> None:
    """Best-effort bounded shutdown for a standalone helper process."""
    if not _process_tree_alive(proc):
        return
    _terminate_process_group(proc)
    if _wait_until_exited([proc], time.monotonic() + max(0.0, graceful_wait_s)):
        return
    if _process_tree_alive(proc):
        _kill_process_group(proc)
    _wait_until_exited([proc], time.monotonic() + max(0.0, kill_wait_s))


def terminate_running_processes(
    running: List[RunningProcess],
    *,
    log_fn: Callable[[str], None],
    kill_wait_s: float = 2.0,
) -> bool:
    """Stop children without allowing supervisor shutdown to hang forever.

    The caller owns the `running` list. This function clears it after closing
    log files and reports whether every process tree actually stopped.
    """
    for item in list(running):
        _set_limiter_stop(item)

    if not running:
        return True

    log_fn("Stopping child processes...")

    items = list(running)
    for item in reversed(items):
        _terminate_process_group(item.process)

    stop_timeout_s = max((float(item.spec.stop_timeout_s) for item in items), default=1.0)
    stop_timeout_s = max(0.2, stop_timeout_s)
    _wait_until_exited([item.process for item in items], time.monotonic() + stop_timeout_s)

    alive = [item for item in items if _process_tree_alive(item.process)]
    if alive:
        log_fn(
            "Force killing unresponsive child processes: "
            + ", ".join(item.spec.name for item in alive)
        )
        for item in reversed(alive):
            _kill_process_group(item.process)
        _wait_until_exited([item.process for item in alive], time.monotonic() + max(0.2, kill_wait_s))

    still_alive = [item for item in items if _process_tree_alive(item.process)]
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
        cleanup_ble_connections(item, log_fn=log_fn)
        try:
            item.log_file.close()
        except Exception:
            pass

    running.clear()
    return not still_alive
