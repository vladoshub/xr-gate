#!/usr/bin/env python3
"""Configure a packaged OpenVR driver; optionally launch Steam or SteamVR."""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import re
import subprocess
import sys


def log(message):
    print(f"[openvr_helper] {message}", file=sys.stderr, flush=True)


def read_json(path):
    return json.loads(path.read_text(encoding="utf-8-sig"))


def package_root():
    explicit = os.environ.get("XR_PACKAGE_ROOT") or os.environ.get("XR_ROOT_PROJECT")
    roots = [Path(explicit).expanduser()] if explicit else list(Path(__file__).resolve().parents)
    for root in roots:
        for candidate in (root, root / "out/xr-gate", root / "out/xreal_ultra"):
            if (candidate / "bin/drivers").is_dir():
                return candidate.resolve()
    raise ValueError("Runtime package not found; set XR_PACKAGE_ROOT to the release directory.")


def select_driver(root, hz, mode):
    base = Path(os.environ.get("XR_OPENVR_DRIVERS_ROOT", str(root / "bin/drivers"))).expanduser()
    profile = next((os.environ[k] for k in ("XR_OPENVR_DEVICE", "XR_DEVICE_TARGET", "XR_TARGET_DEVICE")
                    if os.environ.get(k)), "").lower().replace("-", "_")
    if profile == "xreal_air2ultra":
        profile = "xreal_ultra"
    explicit = os.environ.get("DRIVER_PACKAGE")
    name = os.environ.get("XR_OPENVR_DRIVER_DIR_NAME")
    if explicit:
        candidates = [Path(explicit).expanduser()]
    else:
        candidates = sorted(set(base.glob("*/xr_tracking")) | set(base.glob("*/*/xr_tracking")))
    matches = []
    for driver in candidates:
        settings = driver / "resources/settings/default.vrsettings"
        if not settings.is_file() or not (driver / "driver.vrdrivermanifest").is_file():
            continue
        xr = read_json(settings).get("xr_tracking", {})
        device = xr.get("deviceProfile", "")
        if profile and device != profile:
            continue
        if name and driver.parent.name != name:
            continue
        package_hz = float(xr.get("displayFrequency", 0))
        if not math.isfinite(package_hz) or not 1 <= package_hz <= 1000:
            continue
        matches.append((driver, device, bool(xr.get("isDisplayOnDesktop", False)), package_hz))
    if not matches:
        raise ValueError(f"No matching driver package for profile {profile or '<automatic>'} in {base}.")
    # A registered profile is authoritative when several devices are installed.
    if not profile and len({m[1] for m in matches}) > 1:
        registry = Path(os.environ.get("XR_OPENVRPATHS_FILE", str(Path.home() / ".config/openvr/openvrpaths.vrpath")))
        registered = read_json(registry).get("external_drivers", []) if registry.exists() else []
        active = set()
        for entry in registered:
            settings = Path(entry).expanduser() / "resources/settings/default.vrsettings"
            if settings.is_file():
                active.add(read_json(settings).get("xr_tracking", {}).get("deviceProfile", ""))
        active &= {m[1] for m in matches}
        if len(active) == 1:
            matches = [m for m in matches if m[1] in active]
        else:
            raise ValueError("Multiple device profiles are installed; set XR_OPENVR_DEVICE=" +
                             "|".join(sorted({m[1] or "<legacy-without-profile>" for m in matches})))
    # Package rates are defaults, not binary capabilities. Resolve the device
    # first, then prefer an exact-rate package without requiring one.
    same_frequency = [m for m in matches if math.isclose(m[3], hz, abs_tol=0.001)]
    matches = same_frequency or matches
    preferred = [m for m in matches if m[2] == (mode == "extended_sbs")]
    matches = preferred or matches
    if not same_frequency:
        # Reuse a stable base package when several rate variants are installed.
        # Registration writes the requested rate to user settings only.
        base_hz = min(m[3] for m in matches)
        matches = [m for m in matches if m[3] == base_hz]
    if len(matches) != 1:
        raise ValueError("Multiple matching driver packages; set DRIVER_PACKAGE to one xr_tracking directory.")
    driver, device, on_desktop, package_hz = matches[0]
    if not math.isclose(package_hz, hz, abs_tol=0.001) or on_desktop != (mode == "extended_sbs"):
        package_mode = "extended_sbs" if on_desktop else "direct"
        log(f"Reusing {package_hz:g} Hz {package_mode} package with runtime overrides: {hz:g} Hz {mode}.")
    return driver.resolve(), device or "generic"


def any_process_running(names):
    for name in names:
        result = subprocess.run(["pgrep", "-u", str(os.getuid()), "-x", name],
                                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        if result.returncode == 0:
            return True
        if result.returncode != 1:
            raise ValueError(f"Could not check whether {name} is running (pgrep failed).")
    return False


def require_steamvr_closed():
    # Avoid racing with vrserver while it reads or writes its configuration.
    if any_process_running(("vrserver", "vrcompositor", "vrmonitor")):
        raise ValueError("Close SteamVR before configuring the driver. Steam can remain open.")


def is_wayland(env):
    return env.get("XDG_SESSION_TYPE") == "wayland" or bool(env.get("WAYLAND_DISPLAY"))


def connected_outputs(query):
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
                size = re.search(r"(\d+)mm x (\d+)mm", header[3])
                rotation = re.search(r"[+-]\d+[+-]\d+ (left|right|inverted)(?:\s|$)", header[3])
                current = dict(name=header[1], primary=bool(re.search(r"\bprimary\b", header[3])),
                               active=bool(geometry), edid_hex="", model="", modes={}, non_desktop=None,
                               mode=None, rate=None, rotation=rotation[1] if rotation else "normal",
                               position=f"{int(geometry[3])}x{int(geometry[4])}" if geometry else None,
                               size=f"{size[1]}x{size[2]} mm" if size else "")
                if geometry:
                    current["mode"] = f"{geometry[1]}x{geometry[2]}"
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
        prop = re.fullmatch(r"\s+non-desktop:\s*([01])\s*", line)
        if prop:
            current["non_desktop"] = prop[1]
        mode = re.match(r"^\s+(\S+)\s+(.+)", line)
        if mode:
            rates = re.findall(r"(?:^|\s)([0-9]+(?:\.[0-9]+)?)[*+]*(?=\s|$)", mode[2])
            if re.match(r"^\d+x\d+", mode[1]) and rates:
                current["modes"][mode[1]] = [float(rate) for rate in rates]
            rate = re.search(r"(?:^|\s)([0-9.]+)\*", mode[2])
            if rate:
                current["mode"], current["rate"] = mode[1], rate[1]
    for output in outputs:
        edid = bytes.fromhex(output.pop("edid_hex"))
        output["edid"] = hashlib.sha256(edid).hexdigest() if len(edid) >= 128 else ""
        for offset in range(54, 126, 18):
            if edid[offset:offset + 5] == b"\x00\x00\x00\xfc\x00":
                output["model"] = edid[offset + 5:offset + 18].decode(
                    "ascii", errors="replace").strip("\x00 \r\n")
                break
    return outputs


def direct_choice_file(env):
    return Path(env.get("XDG_CONFIG_HOME") or str(Path.home() / ".config")) / "xr-gate/steamvr-direct-output.json"


def read_direct_choices(env):
    try:
        data = read_json(direct_choice_file(env))
        if isinstance(data, dict) and isinstance(data.get("devices"), dict):
            return data
    except (OSError, ValueError):
        pass
    return {"version": 1, "devices": {}}


def select_direct_output(env, choose=False):
    outputs = connected_outputs(subprocess.check_output(["xrandr", "--prop"], text=True))
    if not outputs:
        raise ValueError("No connected X11 displays found. Connect the headset and retry.")
    explicit = env.get("XR_OPENVR_OUTPUT") or env.get("XR_OPENVR_DGPU_OUTPUT")
    if explicit and not choose:
        output = next((item for item in outputs if item["name"] == explicit), None)
        if output is None:
            raise ValueError(f"Output {explicit} is not connected; check xrandr --query.")
        return output
    if not choose:
        saved = read_direct_choices(env)["devices"].get(env["XR_OPENVR_DEVICE"], {})
        if isinstance(saved, dict) and saved.get("edid"):
            matches = [item for item in outputs if item["edid"] == saved["edid"]]
            same_port = [item for item in matches if item["name"] == saved.get("output")]
            if len(matches) == 1 or len(same_port) == 1:
                output = (same_port or matches)[0]
                log(f"Using saved headset display: {output['name']}.")
                return output
            log("Saved headset display is unavailable or ambiguous; choose it again.")
    if not sys.stdin.isatty():
        raise ValueError("Direct Mode needs a headset display. Run interactively with --choose-output "
                         "or set XR_OPENVR_OUTPUT=<connector>. Use XR_OPENVR_SET_DISPLAY=0 for manual preparation.")
    log("Select the headset display to release from the desktop for SteamVR Direct Mode:")
    for number, output in enumerate(outputs, 1):
        status = f"{output['mode']} @ {output['rate']} Hz" if output["active"] and output["rate"] else (
            output["mode"] if output["active"] else "desktop off")
        primary = " [primary]" if output["primary"] else ""
        model = f" | {output['model']}" if output["model"] else ""
        log(f"  [{number}] {output['name']}{model}{primary} | {status} | {output['size']}")
    while True:
        try:
            selection = input("Headset display number or connector (blank cancels): ").strip()
        except EOFError as exc:
            raise ValueError("Display selection cancelled.") from exc
        if not selection:
            raise ValueError("Display selection cancelled.")
        if selection.isdigit() and 1 <= int(selection) <= len(outputs):
            return outputs[int(selection) - 1]
        named = next((item for item in outputs if item["name"] == selection), None)
        if named:
            return named
        log("Enter a listed display number or connector name.")


def save_direct_output(env, output):
    choices = read_direct_choices(env)
    if output["edid"]:
        choices["devices"][env["XR_OPENVR_DEVICE"]] = dict(output=output["name"], edid=output["edid"])
    else:
        choices["devices"].pop(env["XR_OPENVR_DEVICE"], None)
        log("Display EDID is unavailable; choose the headset again on the next launch or set XR_OPENVR_OUTPUT.")
    try:
        path = direct_choice_file(env)
        path.parent.mkdir(parents=True, exist_ok=True)
        temporary = path.with_name(path.name + f".{os.getpid()}.tmp")
        try:
            temporary.write_text(json.dumps(choices, indent=2) + "\n")
            temporary.replace(path)
        finally:
            temporary.unlink(missing_ok=True)
    except OSError as exc:
        log(f"Could not save the display choice: {exc}")


def restore_direct_output(output):
    log(f"Restoring desktop display {output['name']} after the launch failed.")
    commands = []
    if output["non_desktop"] is not None:
        commands.append(["xrandr", "--output", output["name"], "--set", "non-desktop", output["non_desktop"]])
    command = ["xrandr", "--output", output["name"]]
    if output["active"]:
        command += ["--mode", output["mode"], "--pos", output["position"], "--rotate", output["rotation"]]
        if output["rate"]:
            command += ["--rate", output["rate"]]
        if output["primary"]:
            command += ["--primary"]
    else:
        command += ["--off"]
    commands.append(command)
    for command in commands:
        try:
            if subprocess.run(command).returncode:
                log(f"Could not fully restore {output['name']}; check desktop display settings.")
        except OSError as exc:
            log(f"Could not restore {output['name']}: {exc}")


def set_window_position_from_output(output, env):
    query = subprocess.check_output(["xrandr", "--query"], text=True)
    if not re.search(r"^" + re.escape(output) + r" connected(?:\s|$)", query, re.M):
        raise ValueError(f"Output {output} is not connected; check xrandr --query.")
    match = re.search(r"^" + re.escape(output) + r" connected(?: primary)? \d+x\d+([+-]\d+)([+-]\d+)", query, re.M)
    if not match:
        raise ValueError(f"Cannot determine desktop coordinates for {output}; enable the display first.")
    env["XR_OPENVR_WINDOW_X"], env["XR_OPENVR_WINDOW_Y"] = match.groups()


def prepare_display(mode, hz, env):
    output = env.get("XR_OPENVR_OUTPUT") or env.get("XR_OPENVR_DGPU_OUTPUT")
    if is_wayland(env):
        if mode == "extended_sbs":
            log("Wayland: windowed SBS is experimental; configure display mode/placement in your compositor.")
        if output:
            raise ValueError("Automatic output configuration uses X11 xrandr. On Wayland omit XR_OPENVR_OUTPUT/XR_OPENVR_DGPU_OUTPUT.")
    if not output or env.get("XR_OPENVR_SET_DISPLAY", "1") == "0":
        return
    query = subprocess.check_output(["xrandr", "--query"], text=True)
    if not re.search(r"^" + re.escape(output) + r" connected(?:\s|$)", query, re.M):
        raise ValueError(f"Output {output} is not connected; check xrandr --query.")
    if mode == "direct":
        subprocess.run(["xrandr", "--output", output, "--set", "non-desktop", "1"], check=True)
        subprocess.run(["xrandr", "--output", output, "--off"], check=True)
        return
    # Use driver geometry; there is no XREAL-specific resolution in this launcher.
    xr = read_json(Path(env["DRIVER_PACKAGE"]) / "resources/settings/default.vrsettings")["xr_tracking"]
    resolution = env.get("XR_OPENVR_DGPU_MODE", f"{xr['windowWidth']}x{xr['windowHeight']}")
    subprocess.run(["xrandr", "--output", output, "--set", "non-desktop", "0"], check=True)
    subprocess.run(["xrandr", "--output", output, "--mode", resolution, "--rate", f"{hz:g}"], check=True)
    set_window_position_from_output(output, env)


def gpu_environment(env):
    vendor = env.get("XR_OPENVR_GPU_VENDOR", env.get("XR_OPENVR_DGPU_VENDOR", "auto")).lower()
    if vendor not in ("auto", "nvidia", "amd"):
        raise ValueError("XR_OPENVR_GPU_VENDOR must be auto, nvidia, or amd.")
    if vendor == "auto":
        return  # Preserve the desktop's GPU selection and Vulkan environment.
    explicit = (env.get("XR_OPENVR_VK_ICD_FILENAMES") or env.get("VK_DRIVER_FILES") or
                env.get("VK_ICD_FILENAMES"))
    patterns = ("*nvidia*.json",) if vendor == "nvidia" else ("radeon_icd*.json",)
    paths = [p for folder in ("/usr/share/vulkan/icd.d", "/etc/vulkan/icd.d")
             for pattern in patterns for p in sorted(Path(folder).glob(pattern))]
    # Keep the installed 32/64-bit manifests so Steam's child processes can
    # load the architecture they need. File naming varies between distros.
    icd = explicit or os.pathsep.join(str(path) for path in paths)
    if not icd:
        raise ValueError(f"No {vendor} Vulkan ICD found; set XR_OPENVR_VK_ICD_FILENAMES.")
    env["VK_ICD_FILENAMES"] = env["VK_DRIVER_FILES"] = icd
    if vendor == "nvidia":
        env["__GLX_VENDOR_LIBRARY_NAME"] = "nvidia"
    elif env.get("XR_OPENVR_DRI_PRIME"):
        env["DRI_PRIME"] = env["XR_OPENVR_DRI_PRIME"]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("frequency", nargs="?", help="Refresh rate in Hz (default: 60)")
    parser.add_argument("--choose-output", action="store_true", help="Choose the Direct Mode headset display again (X11)")
    launch_options = parser.add_mutually_exclusive_group()
    launch_options.add_argument("--steam", dest="launch", action="store_const", const="steam",
                                help="Start Steam after registration")
    launch_options.add_argument("--steamvr", dest="launch", action="store_const", const="steamvr",
                                help="Start SteamVR after registration")
    args = parser.parse_args()
    raw_mode = os.environ.get("XR_OPENVR_DISPLAY_MODE", "direct").lower().replace("-", "_")
    mode = {"extended": "extended_sbs", "sbs": "extended_sbs", "windowed": "extended_sbs",
            "desktop": "extended_sbs", "direct_mode": "direct"}.get(raw_mode, raw_mode)
    if mode not in ("direct", "extended_sbs"):
        raise ValueError("XR_OPENVR_DISPLAY_MODE must be direct or extended_sbs.")
    hz = float(os.environ.get("XR_OPENVR_DISPLAY_FREQUENCY_HZ") or
               os.environ.get("XR_DISPLAY_FREQUENCY_HZ") or args.frequency or "60")
    if not math.isfinite(hz) or not 1 <= hz <= 1000:
        raise ValueError("Display frequency must be finite and in range 1..1000 Hz.")
    launch = args.launch or os.environ.get("XR_OPENVR_LAUNCH_MODE", "none").lower()
    if launch not in ("steamvr", "steam", "none"):
        raise ValueError("XR_OPENVR_LAUNCH_MODE must be steamvr, steam, or none.")
    if args.choose_output and (mode != "direct" or launch == "none" or os.environ.get("XR_OPENVR_SET_DISPLAY") == "0"):
        raise ValueError("--choose-output requires Direct Mode, a Steam/SteamVR launch, and display preparation enabled.")
    root = package_root()
    driver, profile = select_driver(root, hz, mode)
    # Prefer the current shared registrar over older scripts embedded in variants.
    registrars = [root / "bin/scripts/drivers/openvr_driver/register_driver.sh",
                  root / "drivers/openvr_driver/scripts/register_driver.sh",
                  driver.parent / "scripts/register_driver.sh"]
    if env_script := os.environ.get("XR_OPENVR_REGISTER_SCRIPT"):
        explicit_script = Path(env_script).expanduser()
        if not explicit_script.is_file():
            raise ValueError(f"XR_OPENVR_REGISTER_SCRIPT not found: {explicit_script}")
        registrars = [explicit_script]
    registrar = next((p for p in registrars if p.is_file()), None)
    if registrar is None:
        raise ValueError("OpenVR registration script not found.")
    env = dict(os.environ, XR_PACKAGE_ROOT=str(root), DRIVER_PACKAGE=str(driver),
               XR_OPENVR_DEVICE=profile, XR_OPENVR_DISPLAY_MODE=mode,
               XR_OPENVR_DISPLAY_FREQUENCY_HZ=f"{hz:g}", XR_DISPLAY_FREQUENCY_HZ=f"{hz:g}",
               XR_OPENVR_PRESERVE_WINDOW_POSITION="1")
    # Validate output requests before changing anything.
    wayland = is_wayland(env)
    if wayland and (
            env.get("XR_OPENVR_OUTPUT") or env.get("XR_OPENVR_DGPU_OUTPUT") or args.choose_output):
        raise ValueError("On Wayland configure displays in your compositor and omit the output override.")
    prepare_direct = mode == "direct" and launch != "none" and env.get("XR_OPENVR_SET_DISPLAY", "1") != "0" and not wayland
    if prepare_direct and not env.get("DISPLAY"):
        raise ValueError("Direct display preparation requires an X11 DISPLAY. Use XR_OPENVR_SET_DISPLAY=0 for manual preparation.")
    windowed_wayland = mode == "extended_sbs" and wayland
    if windowed_wayland and launch != "none" and not env.get("DISPLAY"):
        raise ValueError("Wayland windowed SBS requires XWayland (DISPLAY is unset). "
                         "Enable XWayland or use XR_OPENVR_LAUNCH_MODE=none to register only.")
    require_steamvr_closed()
    if launch != "none":
        steam_running = any_process_running(("steam", "steamwebhelper"))
        if steam_running and windowed_wayland:
            raise ValueError("Close Steam completely before launching windowed SBS through XWayland.")
        if steam_running:
            log("Steam is already running; it may not inherit GPU or display environment overrides.")
        gpu_environment(env)
    direct_output = None
    try:
        if prepare_direct:
            direct_output = select_direct_output(env, args.choose_output)
            env["XR_OPENVR_OUTPUT"] = direct_output["name"]
            log(f"Preparing {direct_output['name']} for Direct Mode (non-desktop=1, desktop output off).")
        if launch == "none" and mode == "extended_sbs":
            output = env.get("XR_OPENVR_OUTPUT") or env.get("XR_OPENVR_DGPU_OUTPUT")
            if output:
                # Registration reads the existing position without changing display settings.
                set_window_position_from_output(output, env)
        elif mode != "direct" or prepare_direct:
            prepare_display(mode, hz, env)
        elif mode == "direct" and wayland:
            log("Wayland Direct Mode requires DRM leasing support from the desktop compositor and GPU driver; XWayland cannot prepare physical outputs.")
        log(f"driver={driver} device={profile} mode={mode} frequency={hz:g}Hz")
        subprocess.run(["bash", str(registrar)], env=env, check=True)
        if launch == "none":
            log("Driver configured and registered. Steam and SteamVR were not started.")
            if windowed_wayland:
                log("Wayland: XWayland windowed SBS is experimental. Registration does not save the launch environment. "
                    "For manual launches, fully exit Steam, then run: "
                    "env -u WAYLAND_DISPLAY XDG_SESSION_TYPE=x11 SDL_VIDEODRIVER=x11 steam")
            return
        if direct_output:
            save_direct_output(env, direct_output)
        if windowed_wayland:
            # Apply compatibility variables only to the fresh Steam process.
            env.pop("WAYLAND_DISPLAY", None)
            env["XDG_SESSION_TYPE"] = "x11"
            env["SDL_VIDEODRIVER"] = "x11"
            log("Starting Steam through XWayland for windowed SBS; performance remains experimental.")
        steam_args = ["steam"] + (["steam://rungameid/250820"] if launch == "steamvr" else [])
        os.execvpe(steam_args[0], steam_args, env)
    except BaseException:
        if direct_output:
            restore_direct_output(direct_output)
        raise


if __name__ == "__main__":
    try:
        main()
    except (ValueError, OSError, subprocess.SubprocessError) as exc:
        log(f"ERROR: {exc}")
        sys.exit(1)
