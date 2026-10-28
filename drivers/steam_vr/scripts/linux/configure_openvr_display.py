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


def is_wayland(env):
    return env.get("XDG_SESSION_TYPE") == "wayland" or bool(env.get("WAYLAND_DISPLAY"))


def connected_outputs(query):
    """Read display names, EDIDs and available modes from xrandr --prop."""
    outputs = []
    current = None
    reading_edid = False
    for line in query.splitlines():
        header = re.match(r"^(\S+) (connected|disconnected)(?:\s|$)(.*)", line)
        if header:
            current = None
            reading_edid = False
            if header[2] == "connected":
                geometry = re.search(r"(\d+)x(\d+)([+-]\d+)([+-]\d+)", header[3])
                current = dict(name=header[1], primary=bool(re.search(r"\bprimary\b", header[3])),
                               active=bool(geometry), edid_hex="", model="", modes={},
                               mode=f"{geometry[1]}x{geometry[2]}" if geometry else None)
                outputs.append(current)
            continue
        if current is None:
            continue
        if reading_edid:
            edid_line = re.fullmatch(r"\s+([0-9a-fA-F]{32})\s*", line)
            if edid_line:
                current["edid_hex"] += edid_line[1]
                continue
            reading_edid = False
        if re.fullmatch(r"\s+EDID:\s*", line):
            reading_edid = True
        mode = re.match(r"^\s+(\S+)\s+(.+)", line)
        if mode:
            rates = re.findall(r"(?:^|\s)([0-9]+(?:\.[0-9]+)?)[*+]*(?=\s|$)", mode[2])
            if re.match(r"^\d+x\d+", mode[1]) and rates:
                current["modes"][mode[1]] = [float(rate) for rate in rates]
            if re.search(r"(?:^|\s)[0-9.]+\*", mode[2]):
                current["mode"] = mode[1]
    for output in outputs:
        edid = bytes.fromhex(output.pop("edid_hex"))
        for offset in range(54, 126, 18):
            if edid[offset:offset + 5] == b"\x00\x00\x00\xfc\x00":
                output["model"] = edid[offset + 5:offset + 18].decode(
                    "ascii", errors="replace").strip("\x00 \r\n")
                break
    return outputs


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
    # XRandR mode names may include a rate suffix (for example a mode created
    # with --newmode). Match the requested geometry and rate, then preserve
    # the actual mode name for both xrandr and the SteamVR launcher.
    resolution_modes = {
        name: rates for name, rates in headset["modes"].items()
        if re.fullmatch(re.escape(args.resolution) + r"(?:[_.-].*)?", name)
    }
    if not resolution_modes:
        available = ", ".join(headset["modes"]) or "none"
        raise ValueError(f"{headset['name']} does not advertise {args.resolution}. "
                         f"Enable the headset's SBS mode first. Available modes: {available}.")
    matches = [name for name, rates in resolution_modes.items()
               if any(math.isclose(rate, args.frequency, abs_tol=0.15) for rate in rates)]
    if not matches:
        available = ", ".join(f"{rate:g}" for rate in sorted({
            rate for rates in resolution_modes.values() for rate in rates
        }))
        raise ValueError(f"{args.resolution} does not offer {args.frequency:g} Hz. Available rates: {available} Hz.")
    mode_name = min(matches, key=lambda name: (name != args.resolution, name != headset["mode"]))
    command = ["xrandr", "--output", monitor["name"], "--primary", "--pos", "0x0",
               "--output", headset["name"], "--mode", mode_name,
               "--rate", f"{args.frequency:g}", "--right-of", monitor["name"]]
    print("\nApplying: " + shlex.join(command), flush=True)
    subprocess.run(command, check=True)
    print(f"\nPlaced {headset['model'] or headset['name']} to the right of "
          f"{monitor['model'] or monitor['name']}.")
    print("To register the driver with matching window coordinates and refresh rate, run:")
    print(shlex.join(["env", "XR_OPENVR_DISPLAY_MODE=extended_sbs",
                      f"XR_OPENVR_OUTPUT={headset['name']}",
                      f"XR_OPENVR_DISPLAY_FREQUENCY_HZ={args.frequency:g}",
                      f"XR_DISPLAY_FREQUENCY_HZ={args.frequency:g}",
                      f"XR_OPENVR_DGPU_MODE={mode_name}", "./openvr_helper.sh"]))


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("\nDisplay selection cancelled.", file=sys.stderr)
        sys.exit(130)
    except (ValueError, OSError, subprocess.SubprocessError) as exc:
        print(f"[configure_openvr_display] ERROR: {exc}", file=sys.stderr)
        sys.exit(1)
