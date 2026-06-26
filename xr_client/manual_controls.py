#!/usr/bin/env python3
"""Interactive keyboard controls for xr_backend_client.

POSIX/WSL uses non-blocking ``select`` over ``stdin`` from the main supervisor
loop. Native Windows cannot use ``select`` on console handles, so it uses
``msvcrt.kbhit()/getwch()`` instead. No background ``input()`` thread is used;
therefore console controls cannot keep the supervisor alive during shutdown.
"""
from __future__ import annotations

import os
import sys
import time
from typing import Dict, List

try:
    from .common import env_truthy, log
    from .tap_controls import TapEvent
except ImportError:
    from common import env_truthy, log
    from tap_controls import TapEvent


class ManualControlPoller:
    def __init__(self) -> None:
        self.stop_requested = False
        self._win_buffer = ""
        self._prompt = "XR control [1/2/3/4/5/6/7/8/h/q]: "
        self.mapping: Dict[str, str] = {
            "1": "manual-restart-running-backends",
            "r": "manual-restart-running-backends",
            "restart": "manual-restart-running-backends",
            "2": "manual-toggle-hand-tracking",
            "htr": "manual-toggle-hand-tracking",
            "hand": "manual-toggle-hand-tracking",
            "hands": "manual-toggle-hand-tracking",
            "3": "manual-toggle-3dof-6dof",
            "3dof": "manual-toggle-3dof-6dof",
            "6dof": "manual-toggle-3dof-6dof",
            "toggle": "manual-toggle-3dof-6dof",
            "4": "manual-recenter-3dof",
            "recenter": "manual-recenter-3dof",
            "5": "manual-toggle-override-controller",
            "oc": "manual-toggle-override-controller",
            "override": "manual-toggle-override-controller",
            "controller": "manual-toggle-override-controller",
            "6": "manual-toggle-xr-video",
            "video": "manual-toggle-xr-video",
            "xr_video": "manual-toggle-xr-video",
            "xrv": "manual-toggle-xr-video",
            "7": "manual-toggle-xr-spatial",
            "spatial": "manual-toggle-xr-spatial",
            "xr_spatial": "manual-toggle-xr-spatial",
            "xrs": "manual-toggle-xr-spatial",
            "8": "manual-toggle-shake-controls",
            "shake": "manual-toggle-shake-controls",
            "shakes": "manual-toggle-shake-controls",
            "shake_controls": "manual-toggle-shake-controls",
            "tap": "manual-toggle-shake-controls",
            "taps": "manual-toggle-shake-controls",
            "tap_controls": "manual-toggle-shake-controls",
            "imu": "manual-toggle-shake-controls",
            "imu_tap": "manual-toggle-shake-controls",
        }

    @staticmethod
    def available() -> bool:
        if not env_truthy("RUN_MANUAL_CONTROLS", True):
            log("Manual backend controls disabled")
            return False
        if not sys.stdin.isatty():
            log("Manual backend controls disabled: stdin is not a TTY")
            return False
        return True

    def _print_prompt(self) -> None:
        print(self._prompt, end="", flush=True)

    def print_menu(self) -> None:
        print("", flush=True)
        print("XR backend controls:", flush=True)
        print("  1 - restart running backends", flush=True)
        print("  2 - start/stop hand_tracking", flush=True)
        print("  3 - toggle 3DoF/6DoF", flush=True)
        print("  4 - recenter 3DoF (works only when imu_3dof_backend is running)", flush=True)
        print("  5 - start/stop override_controller", flush=True)
        print("  6 - start/stop xr_video", flush=True)
        print("  7 - start/stop xr_spatial", flush=True)
        print("  8 - shake control toggle (enable/disable IMU tap controls for this session)", flush=True)
        print("  h - show this menu", flush=True)
        print("  q - stop all and exit", flush=True)
        print("", flush=True)
        self._print_prompt()

    def _handle_command(self, raw: str) -> List[TapEvent]:
        raw = raw.strip().lower()
        if not raw:
            self._print_prompt()
            return []
        if raw in ("h", "help", "?"):
            self.print_menu()
            return []
        if raw in ("q", "quit", "exit", "stop"):
            self.stop_requested = True
            print("Stopping...", flush=True)
            return []
        event_name = self.mapping.get(raw)
        if not event_name:
            print("Unknown command. Use 1, 2, 3, 4, 5, 6, 7, 8, h or q.", flush=True)
            self._print_prompt()
            return []
        self._print_prompt()
        return [TapEvent(name=event_name, side="manual", confidence=1.0, timestamp_ns=time.monotonic_ns())]

    def poll_events(self) -> List[TapEvent]:
        if os.name == "posix":
            return self._poll_posix()
        if os.name == "nt":
            return self._poll_windows()
        return []

    def _poll_posix(self) -> List[TapEvent]:
        try:
            import select
            ready, _, _ = select.select([sys.stdin], [], [], 0.0)
        except Exception:
            return []
        if not ready:
            return []
        line = sys.stdin.readline()
        if line == "":
            self.stop_requested = True
            return []
        return self._handle_command(line)

    def _poll_windows(self) -> List[TapEvent]:
        """Poll native Windows console without select() or input() threads.

        Single-key controls (1/2/3/4/5/6/7/8/h/q) are executed immediately. Longer text
        aliases still work after Enter. Printable characters are echoed manually
        because msvcrt.getwch() reads raw console input.
        """
        try:
            import msvcrt
        except Exception:
            return []

        events: List[TapEvent] = []
        single_key_commands = {"1", "2", "3", "4", "5", "6", "7", "8", "h", "q", "r"}
        while msvcrt.kbhit():
            ch = msvcrt.getwch()

            # Function/cursor keys are delivered as a prefix plus a second code.
            if ch in ("\x00", "\xe0"):
                if msvcrt.kbhit():
                    try:
                        msvcrt.getwch()
                    except Exception:
                        pass
                continue

            # Ctrl+C / Ctrl+Z should stop the supervisor cleanly.
            if ch in ("\x03", "\x1a"):
                self.stop_requested = True
                print("^C", flush=True)
                break

            if ch in ("\r", "\n"):
                print("", flush=True)
                command = self._win_buffer
                self._win_buffer = ""
                events.extend(self._handle_command(command))
                continue

            if ch in ("\b", "\x7f"):
                if self._win_buffer:
                    self._win_buffer = self._win_buffer[:-1]
                    print("\b \b", end="", flush=True)
                continue

            # Execute the common one-character commands immediately when the
            # user has not started typing a longer alias.
            lower = ch.lower()
            if not self._win_buffer and lower in single_key_commands:
                print(ch, flush=True)
                events.extend(self._handle_command(lower))
                continue

            if ch.isprintable():
                self._win_buffer += ch
                print(ch, end="", flush=True)

        return events
