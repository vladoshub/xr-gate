#!/usr/bin/env python3
"""Launch a packaged OpenVR driver without assuming a GPU or display connector."""
import json
import math
import os
from pathlib import Path
import re
import subprocess
import sys
import time


def log(message):
    print(f"[start_openvr] {message}", file=sys.stderr, flush=True)


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
        if not math.isclose(float(xr.get("displayFrequency", 0)), hz, abs_tol=0.001):
            continue
        matches.append((driver, device, bool(xr.get("isDisplayOnDesktop", False))))
    if not matches:
        raise ValueError(f"No matching {hz:g} Hz driver for profile {profile or '<automatic>'} in {base}.")
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
    preferred = [m for m in matches if m[2] == (mode == "extended_sbs")]
    matches = preferred or matches
    if len(matches) != 1:
        raise ValueError("Multiple matching driver packages; set DRIVER_PACKAGE to one xr_tracking directory.")
    driver, device, on_desktop = matches[0]
    if on_desktop != (mode == "extended_sbs"):
        log("Using the same-frequency driver with runtime display-mode overrides.")
    return driver.resolve(), device or "generic"


def stop_steam():
    names = ("steam", "steamwebhelper", "vrserver", "vrcompositor", "vrmonitor")

    def running():
        return any(subprocess.run(["pgrep", "-u", str(os.getuid()), "-x", name],
                                  stdout=subprocess.DEVNULL).returncode == 0 for name in names)

    if not running():
        return
    if os.environ.get("XR_OPENVR_STOP_STEAM", "auto").lower() in ("0", "false", "no", "off"):
        raise ValueError("Close SteamVR and Steam before changing display settings.")
    log("Closing Steam before updating display settings.")
    subprocess.run(["steam", "-shutdown"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=15)
    deadline = time.monotonic() + 15
    while running():
        if time.monotonic() >= deadline:
            raise ValueError("Steam/SteamVR is still running; close it and retry.")
        time.sleep(0.5)


def prepare_display(mode, hz, env):
    output = env.get("XR_OPENVR_OUTPUT") or env.get("XR_OPENVR_DGPU_OUTPUT")
    wayland = env.get("XDG_SESSION_TYPE") == "wayland" or bool(env.get("WAYLAND_DISPLAY"))
    if wayland:
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
    query = subprocess.check_output(["xrandr", "--query"], text=True)
    match = re.search(r"^" + re.escape(output) + r" connected(?: primary)? \d+x\d+([+-]\d+)([+-]\d+)", query, re.M)
    if not match:
        raise ValueError(f"Cannot determine desktop coordinates for {output}.")
    env["XR_OPENVR_WINDOW_X"], env["XR_OPENVR_WINDOW_Y"] = match.groups()


def gpu_environment(env):
    vendor = env.get("XR_OPENVR_GPU_VENDOR", env.get("XR_OPENVR_DGPU_VENDOR", "auto")).lower()
    if vendor not in ("auto", "nvidia", "amd"):
        raise ValueError("XR_OPENVR_GPU_VENDOR must be auto, nvidia, or amd.")
    if vendor == "auto":
        return  # Preserve the desktop's GPU selection and Vulkan environment.
    explicit = env.get("XR_OPENVR_VK_ICD_FILENAMES") or env.get("VK_ICD_FILENAMES")
    patterns = ("*nvidia*.json",) if vendor == "nvidia" else ("radeon_icd.x86_64.json",)
    paths = [p for folder in ("/usr/share/vulkan/icd.d", "/etc/vulkan/icd.d")
             for pattern in patterns for p in sorted(Path(folder).glob(pattern))]
    icd = explicit or (str(paths[0]) if paths else "")
    if not icd:
        raise ValueError(f"No {vendor} Vulkan ICD found; set XR_OPENVR_VK_ICD_FILENAMES.")
    env["VK_ICD_FILENAMES"] = env["VK_DRIVER_FILES"] = icd
    if vendor == "nvidia":
        env["__GLX_VENDOR_LIBRARY_NAME"] = "nvidia"
    elif env.get("XR_OPENVR_DRI_PRIME"):
        env["DRI_PRIME"] = env["XR_OPENVR_DRI_PRIME"]


def main():
    raw_mode = os.environ.get("XR_OPENVR_DISPLAY_MODE", "direct").lower().replace("-", "_")
    mode = {"extended": "extended_sbs", "sbs": "extended_sbs", "windowed": "extended_sbs",
            "desktop": "extended_sbs", "direct_mode": "direct"}.get(raw_mode, raw_mode)
    if mode not in ("direct", "extended_sbs"):
        raise ValueError("XR_OPENVR_DISPLAY_MODE must be direct or extended_sbs.")
    hz = float(os.environ.get("XR_OPENVR_DISPLAY_FREQUENCY_HZ") or
               os.environ.get("XR_DISPLAY_FREQUENCY_HZ") or (sys.argv[1] if len(sys.argv) == 2 else "60"))
    if not math.isfinite(hz) or not 1 <= hz <= 1000:
        raise ValueError("Display frequency must be finite and in range 1..1000 Hz.")
    launch = os.environ.get("XR_OPENVR_LAUNCH_MODE", "steamvr").lower()
    if launch not in ("steamvr", "steam", "none"):
        raise ValueError("XR_OPENVR_LAUNCH_MODE must be steamvr, steam, or none.")
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
    gpu_environment(env)
    # Validate Wayland output requests before closing any application.
    if (env.get("XDG_SESSION_TYPE") == "wayland" or env.get("WAYLAND_DISPLAY")) and (
            env.get("XR_OPENVR_OUTPUT") or env.get("XR_OPENVR_DGPU_OUTPUT")):
        raise ValueError("On Wayland configure displays in your compositor and omit the output override.")
    stop_steam()
    prepare_display(mode, hz, env)
    log(f"driver={driver} device={profile} mode={mode} frequency={hz:g}Hz")
    subprocess.run(["bash", str(registrar)], env=env, check=True)
    if launch == "none":
        log("Configuration complete; launch skipped.")
        return
    args = ["steam"] + (["steam://rungameid/250820"] if launch == "steamvr" else [])
    os.execvpe(args[0], args, env)


if __name__ == "__main__":
    try:
        main()
    except (ValueError, OSError, subprocess.SubprocessError) as exc:
        log(f"ERROR: {exc}")
        sys.exit(1)
