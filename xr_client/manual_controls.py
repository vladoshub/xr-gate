#!/usr/bin/env python3
"""Interactive keyboard controls for xr_backend_client.

POSIX/WSL uses cbreak mode and non-blocking ``select`` over ``stdin`` from the
main supervisor loop. Native Windows cannot use ``select`` on console handles, so it uses
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
        self._input_buffer = ""
        self._posix_fd = None
        self._posix_terminal_state = None
        if os.name == "posix" and sys.stdin.isatty():
            try:
                import termios
                import tty
                fd = sys.stdin.fileno()
                state = termios.tcgetattr(fd)
                tty.setcbreak(fd, termios.TCSANOW)
                self._posix_fd = fd
                self._posix_terminal_state = state
            except (OSError, ValueError, termios.error):
                pass  # Retain line-based controls if this TTY cannot use cbreak.
        self._prompt = "XR control [1/2/3/4/5/6/7/8/9/h/q]: "
        self.mapping: Dict[str, str] = {
            "1": "manual-restart-running-backends",
            "r": "manual-restart-running-backends",
            "restart": "manual-restart-running-backends",
            "2": "manual-toggle-hand-tracking",
            "htr": "manual-toggle-hand-tracking",
            "hand": "manual-toggle-hand-tracking",
            "hands": "manual-toggle-hand-tracking",
            "3": "manual-toggle-basalt-imu-mode",
            "basalt": "manual-toggle-basalt-imu-mode",
            "basalt-imu": "manual-toggle-basalt-imu-mode",
            "no-imu": "manual-toggle-basalt-imu-mode",
            "vo": "manual-toggle-basalt-imu-mode",
            "vio": "manual-toggle-basalt-imu-mode",
            "9": "manual-toggle-3dof-6dof",
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

    def close(self) -> None:
        """Restore the user's terminal after normal exit or a stop signal."""
        if self._posix_fd is not None and self._posix_terminal_state is not None:
            import termios
            try:
                termios.tcsetattr(self._posix_fd, termios.TCSANOW, self._posix_terminal_state)
            except (OSError, ValueError, termios.error):
                pass
            self._posix_fd = None
            self._posix_terminal_state = None

    def read_line(self, prompt: str) -> str:
        """Temporarily restore line editing for an interactive recovery prompt."""
        if self._posix_fd is None:
            return input(prompt)
        import termios
        import tty
        termios.tcsetattr(self._posix_fd, termios.TCSANOW, self._posix_terminal_state)
        try:
            return input(prompt)
        finally:
            tty.setcbreak(self._posix_fd, termios.TCSANOW)

    def redraw_prompt(self) -> None:
        self._print_prompt()
        if self._input_buffer:
            print(self._input_buffer, end="", flush=True)

    def print_menu(self) -> None:
        print("", flush=True)
        print("XR backend controls:", flush=True)
        print("  1 - restart running backends", flush=True)
        print("  2 - start/stop hand_tracking", flush=True)
        print("  3 - toggle Basalt VIO / stereo VO (restarts Basalt)", flush=True)
        print("  4 - recenter 3DoF (works only when imu_3dof_backend is running)", flush=True)
        print("  5 - start/stop override_controller", flush=True)
        print("  6 - start/stop xr_video", flush=True)
        print("  7 - start/stop xr_spatial", flush=True)
        print("  8 - shake control toggle (enable/disable IMU tap controls for this session)", flush=True)
        print("  9 - toggle 6DoF/3DoF", flush=True)
        print("  h - show this menu", flush=True)
        print("  q - stop all and exit", flush=True)
        if os.name == "posix":
            print("  Keys 1-9 act without Enter; text commands use Enter."
                  if self._posix_fd is not None else "  Type a command and press Enter.", flush=True)
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
            print("Unknown command. Use 1, 2, 3, 4, 5, 6, 7, 8, 9, h or q.", flush=True)
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
        if self._posix_fd is not None:
            data = os.read(self._posix_fd, 4096)
            if not data:
                self.stop_requested = True
                return []
            events = []
            for ch in data.decode("utf-8", errors="replace"):
                events.extend(self._handle_character(ch, set("123456789")))
            return events
        line = sys.stdin.readline()
        if line == "":
            self.stop_requested = True
            return []
        return self._handle_command(line)

    def _handle_character(self, ch: str, single_key_commands: set) -> List[TapEvent]:
        if ch in ("\x03", "\x04", "\x1a"):
            self.stop_requested = True
            print("Stopping...", flush=True)
            return []
        if ch in ("\r", "\n"):
            print("", flush=True)
            command = self._input_buffer
            self._input_buffer = ""
            return self._handle_command(command)
        if ch in ("\b", "\x7f"):
            if self._input_buffer:
                self._input_buffer = self._input_buffer[:-1]
                print("\b \b", end="", flush=True)
            return []
        lower = ch.lower()
        if not self._input_buffer and lower in single_key_commands:
            print(ch, flush=True)
            return self._handle_command(lower)
        if ch.isprintable():
            self._input_buffer += ch
            print(ch, end="", flush=True)
        return []

    def _poll_windows(self) -> List[TapEvent]:
        """Poll native Windows console without select() or input() threads.

        Single-key controls (1/2/3/4/5/6/7/8/9/h/q) are executed immediately. Longer text
        aliases still work after Enter. Printable characters are echoed manually
        because msvcrt.getwch() reads raw console input.
        """
        try:
            import msvcrt
        except Exception:
            return []

        events: List[TapEvent] = []
        single_key_commands = {"1", "2", "3", "4", "5", "6", "7", "8", "9", "h", "q", "r"}
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

            events.extend(self._handle_character(ch, single_key_commands))
            if self.stop_requested:
                break
        return events
