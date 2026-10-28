#!/usr/bin/env python3
"""Interactively place a headset display to the right of a monitor on X11."""
import argparse
import math
import os
import re
import shlex
import shutil
import subprocess
import sys

from start_openvr import connected_outputs, is_wayland


def choose_output(prompt, outputs):
    for number, output in enumerate(outputs, 1):
        model = output["model"] or "EDID name unavailable"
        state = output["mode"] if output["active"] else "desktop off"
        primary = " [primary]" if output["primary"] else ""
        print(f"  [{number}] {model} | {output['name']} | {state}{primary}")
    while True:
        try:
            answer = input(f"{prompt} (name, output or number; Enter cancels): ").strip()
        except EOFError as exc:
            raise ValueError("Display selection cancelled.") from exc
        if not answer:
            raise ValueError("Display selection cancelled.")
        if answer.isdigit() and 1 <= int(answer) <= len(outputs):
            return outputs[int(answer) - 1]
        needle = answer.casefold()
        matches = [item for item in outputs if needle in (
            item["name"].casefold(), item["model"].casefold())]
        if not matches:
            matches = [item for item in outputs if needle in item["model"].casefold()]
        if len(matches) == 1:
            return matches[0]
        if len(matches) > 1:
            print("Several displays match; enter a connector name or number.")
        else:
            print("No matching display; choose one from the list.")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("resolution", metavar="WIDTHxHEIGHT", help="Headset resolution, e.g. 3840x1080")
    parser.add_argument("frequency", nargs="?", type=float, default=60, help="Refresh rate in Hz (default: 60)")
    args = parser.parse_args()
    if not re.fullmatch(r"[1-9]\d*x[1-9]\d*", args.resolution):
        parser.error("Resolution must be WIDTHxHEIGHT, e.g. 3840x1080.")
    if not math.isfinite(args.frequency) or not 1 <= args.frequency <= 1000:
        parser.error("Frequency must be finite and in range 1..1000 Hz.")
    if is_wayland(os.environ):
        raise ValueError("This script configures physical displays on X11. On Wayland use your desktop display settings.")
    if not os.environ.get("DISPLAY"):
        raise ValueError("No X11 DISPLAY. Run this script in an X11 desktop session.")
    if not shutil.which("xrandr"):
        raise ValueError("xrandr is missing. On Ubuntu/Debian install x11-xserver-utils.")
    if not sys.stdin.isatty():
        raise ValueError("Run this interactive script in a terminal.")
    outputs = connected_outputs(subprocess.check_output(["xrandr", "--prop"], text=True))
    if len(outputs) < 2:
        raise ValueError("Connect both the monitor and headset before running this script.")
    monitors = [item for item in outputs if item["active"]]
    if not monitors:
        raise ValueError("Enable the reference monitor in your desktop display settings first.")
    print("Choose the monitor to place the headset to the right of.")
    print("This monitor will become primary at desktop position 0,0.")
    monitor = choose_output("Reference monitor", monitors)
    headsets = [item for item in outputs if item["name"] != monitor["name"]]
    print("\nChoose the headset display.")
    headset = choose_output("Headset", headsets)
    rates = headset["modes"].get(args.resolution, [])
    if not rates:
        available = ", ".join(headset["modes"]) or "none"
        raise ValueError(f"{headset['name']} does not advertise {args.resolution}. "
                         f"Enable the headset's SBS mode first. Available modes: {available}.")
    if not any(math.isclose(rate, args.frequency, abs_tol=0.15) for rate in rates):
        available = ", ".join(f"{rate:g}" for rate in rates)
        raise ValueError(f"{args.resolution} does not offer {args.frequency:g} Hz. Available rates: {available} Hz.")
    command = ["xrandr", "--output", monitor["name"], "--primary", "--pos", "0x0",
               "--output", headset["name"], "--mode", args.resolution,
               "--rate", f"{args.frequency:g}", "--right-of", monitor["name"]]
    print("\nApplying: " + shlex.join(command), flush=True)
    subprocess.run(command, check=True)
    print(f"\nPlaced {headset['model'] or headset['name']} to the right of "
          f"{monitor['model'] or monitor['name']}.")
    print("To start SteamVR with matching window coordinates and refresh rate, run:")
    print(shlex.join(["env", "XR_OPENVR_DISPLAY_MODE=extended_sbs",
                      f"XR_OPENVR_OUTPUT={headset['name']}",
                      f"XR_OPENVR_DISPLAY_FREQUENCY_HZ={args.frequency:g}",
                      f"XR_DISPLAY_FREQUENCY_HZ={args.frequency:g}",
                      f"XR_OPENVR_DGPU_MODE={args.resolution}",
                      "XR_OPENVR_LAUNCH_MODE=steamvr", "./run_openvr.sh"]))


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("\nDisplay selection cancelled.", file=sys.stderr)
        sys.exit(130)
    except (ValueError, OSError, subprocess.SubprocessError) as exc:
        print(f"[configure_openvr_display] ERROR: {exc}", file=sys.stderr)
        sys.exit(1)
