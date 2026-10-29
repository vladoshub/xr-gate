#!/usr/bin/env python3
"""Register an experimental X11 mode using existing SBS timing totals."""
import argparse
import math
import os
import re
import shlex
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", help="Headset XRandR connector, e.g. DP-6")
    parser.add_argument("frequency", type=float, help="Requested refresh rate in Hz")
    parser.add_argument("--resolution", default="3840x1080", help="Headset resolution (default: 3840x1080)")
    args = parser.parse_args()
    if not re.fullmatch(r"[1-9]\d*x[1-9]\d*", args.resolution):
        parser.error("Resolution must be WIDTHxHEIGHT.")
    if not math.isfinite(args.frequency) or not 1 <= args.frequency <= 1000:
        parser.error("Frequency must be finite and in range 1..1000 Hz.")
    if os.environ.get("XDG_SESSION_TYPE") == "wayland" or os.environ.get("WAYLAND_DISPLAY"):
        raise ValueError("Use a native X11 desktop session for this experiment.")
    if not os.environ.get("DISPLAY"):
        raise ValueError("No X11 DISPLAY.")
    query = subprocess.check_output(["xrandr", "--verbose"], text=True)
    sections = re.split(r"(?m)(?=^\S+ (?:dis)?connected\b)", query)
    section = next((part for part in sections
                    if re.match(re.escape(args.output) + r" connected\b", part)), None)
    if section is None:
        raise ValueError(f"{args.output} is not connected; check xrandr --query.")
    mode_name = f"{args.resolution}_{args.frequency:g}_test"
    mode_pattern = (
        r"^[ \t]+(?P<name>" + re.escape(args.resolution) + r"(?:[_.-]\S+)?)"
        r"[ \t]+\([^)]*\)[ \t]+[0-9.]+MHz(?P<flags>[^\n]*)\n"
        r"(?P<h>[^\n]*)\n(?P<v>[^\n]*)"
    )
    modes = list(re.finditer(mode_pattern, section, re.M))
    modes = [mode for mode in modes if not re.search(r"interlace|doublescan", mode["flags"], re.I)]
    if not modes:
        raise ValueError(f"No progressive {args.resolution} timing found. Enable SBS first.")
    existing = next((mode for mode in modes if mode["name"] == mode_name), None)
    if existing:
        raise ValueError(f"{mode_name} is already attached to {args.output}. "
                         "Use the display setup script, or remove this mode before recreating it.")
    base = min(modes, key=lambda mode: ("+preferred" not in mode["flags"],
                                       "*current" not in mode["flags"]))
    horizontal = re.findall(r"\b(?:width|start|end|total)\s+(\d+)", base["h"])
    vertical = re.findall(r"\b(?:height|start|end|total)\s+(\d+)", base["v"])
    flags = re.findall(r"[+-](?:HSync|VSync)", base["flags"], re.I)
    if (len(horizontal) != 4 or len(vertical) != 4 or len(flags) != 2 or
            f"{horizontal[0]}x{vertical[0]}" != args.resolution):
        raise ValueError("Cannot read native timing totals and sync polarity from xrandr --verbose.")
    # Progressive refresh = pixel clock / (horizontal total * vertical total).
    # Keep blanking and sync positions from the existing display mode.
    clock_mhz = args.frequency * int(horizontal[3]) * int(vertical[3]) / 1_000_000
    command = ["xrandr", "--newmode", mode_name, f"{clock_mhz:.6f}",
               *horizontal, *vertical, *flags]
    print(f"Base timing: {base['name']}")
    print("Registering: " + shlex.join(command), flush=True)
    subprocess.run(command, check=True)
    try:
        subprocess.run(["xrandr", "--addmode", args.output, mode_name], check=True)
    except (OSError, subprocess.SubprocessError):
        # --newmode succeeded, but the driver refused to attach it. Remove only
        # the mode created by this invocation so the name can be reused.
        try:
            subprocess.run(["xrandr", "--rmmode", mode_name], check=True)
            print(f"Removed unassociated mode {mode_name}.", file=sys.stderr)
        except (OSError, subprocess.SubprocessError):
            print(f"Could not remove {mode_name}; try: xrandr --rmmode {mode_name}", file=sys.stderr)
        raise
    print(f"Registered {mode_name} on {args.output}. Headset acceptance is unverified.")
    print("Apply the experimental mode with:")
    print(shlex.join(["./configure_openvr_display.sh", args.resolution, f"{args.frequency:g}"]))


if __name__ == "__main__":
    try:
        main()
    except (ValueError, OSError, subprocess.SubprocessError) as exc:
        print(f"[add_openvr_mode] ERROR: {exc}", file=sys.stderr)
        sys.exit(1)
