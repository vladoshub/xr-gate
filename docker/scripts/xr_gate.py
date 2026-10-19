#!/usr/bin/env python3
"""Local Docker/USB orchestration; Python standard library only."""
import argparse
import glob
import json
import os
from pathlib import Path
import re
import shutil
import stat
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
NAME = "xr-gate"


def image(env):
    version = env.get("XR_VERSION", "")
    if not re.fullmatch(r"[A-Za-z0-9_][A-Za-z0-9_.-]{0,127}", version):
        raise ValueError("Set XR_VERSION in .env to a release tag, for example v1.2.3")
    return f"xr-gate-runtime:{version}"


def read_env(path):
    values = {}
    if not path.exists():
        return values
    for number, raw in enumerate(path.read_text(encoding="utf-8-sig").splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        match = re.fullmatch(r"([A-Za-z_][A-Za-z_0-9]*)=(.*)", line)
        if not match:
            raise ValueError(f"{path}:{number}: expected KEY=value")
        key, value = match.groups()
        value = value.strip()
        if value.startswith(('"', "'")):
            if len(value) < 2 or value[-1] != value[0]:
                raise ValueError(f"{path}:{number}: unclosed quote")
            value = value[1:-1]
        if "$" in value or "`" in value:
            raise ValueError(f"{path}:{number}: use literal values, no shell/variable expressions")
        values[key] = value
    return values


def settings():
    # Device-specific defaults belong to the shipped environment profile.
    values = read_env(ROOT / ".env.example")
    values.update(read_env(ROOT / ".env"))
    values.update(os.environ)
    image(values)
    for key in ("XR_BLUETOOTH", "XR_VIDEO_TCP", "XR_REQUIRE_CAMERA", "XR_REQUIRE_HIDRAW"):
        if values.get(key, "0") not in ("0", "1"):
            raise ValueError(f"{key} must be 0 or 1")
    for key, default in (("XR_TCP_PORT", "45810"), ("XR_VIDEO_PORT", "45700")):
        port = int(values.get(key, default))
        if not 1 <= port <= 65535:
            raise ValueError(f"Invalid {key}")
    if values.get("XR_VIDEO_TCP") == "1" and values.get("XR_TCP_PORT", "45810") == values.get("XR_VIDEO_PORT", "45700"):
        raise ValueError("Application and video host ports must differ")
    return values


def call(args, env=None, capture=False, check=True):
    result = subprocess.run([str(x) for x in args], cwd=ROOT, env=env,
                            text=True, capture_output=capture, check=check)
    return result.stdout.strip() if capture else result.returncode


def local_engine(env):
    if not shutil.which("docker"):
        raise RuntimeError("Docker CLI missing. See scripts/install-engine-ubuntu.sh")
    if env.get("DOCKER_HOST") and not env.get("DOCKER_CONTEXT"):
        endpoint = env["DOCKER_HOST"]
    else:
        endpoint = call(["docker", "context", "inspect", "--format", "{{.Endpoints.docker.Host}}"], env, True)
    if not endpoint.startswith("unix://"):
        raise RuntimeError("A local Linux Docker Engine with a unix:// socket is required for USB bind mounts")
    info = json.loads(call(["docker", "info", "--format", "{{json .}}"], env, True))
    if info.get("OSType") != "linux" or "docker desktop" in info.get("OperatingSystem", "").lower():
        raise RuntimeError("Use Docker Engine on the Linux host connected to the devices; see README.md")
    if any("rootless" in x for x in info.get("SecurityOptions", [])):
        raise RuntimeError("USB device access in this bundle requires the regular (rootful) local Docker Engine")
    version = re.match(r"(\d+)", info.get("ServerVersion", ""))
    if not version or int(version.group(1)) < 28:
        raise RuntimeError("Docker Engine >= 28 required for reliable localhost-only publishing")
    call(["docker", "compose", "version"], env, True)
    return info


def usb_identity(path):
    for parent in (path.resolve(), *path.resolve().parents):
        vendor, product = parent / "idVendor", parent / "idProduct"
        if vendor.is_file() and product.is_file():
            return f"{vendor.read_text().strip()}:{product.read_text().strip()}".lower()
    return None


def selected_ids(env):
    values = {x.strip().lower() for x in env.get("XR_USB_IDS", "").split(",") if x.strip()}
    if any(not re.fullmatch(r"[0-9a-f]{4}:[0-9a-f]{4}", x) for x in values):
        raise ValueError("XR_USB_IDS must contain comma-separated four-digit VID:PID pairs")
    return values


def discover(env, sysroot=Path("/sys"), devroot=Path("/dev")):
    identities = selected_ids(env)
    videos = [p for p in (sysroot / "class/video4linux").glob("video*") if usb_identity(p) in identities]
    videos.sort(key=lambda p: (int((p / "index").read_text()) if (p / "index").exists() else 99,
                               int(re.search(r"\d+$", p.name).group())))
    requested = env.get("XR_CAMERA_DEVICE", "")
    if requested:
        if not identities:
            # An explicit camera is sufficient without a vendor selector. Do
            # not discover unrelated cameras or USB/HID nodes in this mode.
            videos = [p for p in (sysroot / "class/video4linux").glob("video*")
                      if str(devroot / p.name) == requested]
        videos = [p for p in videos if str(devroot / p.name) == requested] + [p for p in videos if str(devroot / p.name) != requested]
        if not videos or str(devroot / videos[0].name) != requested:
            raise RuntimeError(f"XR_CAMERA_DEVICE not found or does not belong to XR_USB_IDS: {requested}")
    devices = []
    for index, path in enumerate(videos):
        devices.append((str(devroot / path.name), f"/dev/video{index}"))
    for path in sorted((sysroot / "class/hidraw").glob("hidraw*")):
        if usb_identity(path) in identities:
            devices.append((str(devroot / path.name), f"/dev/{path.name}"))
    for path in sorted((sysroot / "class/tty").glob("tty*")):
        if re.fullmatch(r"tty(?:ACM|USB)\d+", path.name) and usb_identity(path) in identities:
            devices.append((str(devroot / path.name), f"/dev/{path.name}"))
    for path in sorted((sysroot / "bus/usb/devices").glob("*")):
        if usb_identity(path) in identities and (path / "busnum").is_file() and (path / "devnum").is_file():
            node = devroot / "bus/usb" / f'{int((path / "busnum").read_text()):03d}' / f'{int((path / "devnum").read_text()):03d}'
            devices.append((str(node), str(node)))
    for node in filter(None, (p.strip() for p in env.get("XR_INPUT_DEVICES", "").split(","))):
        if not re.fullmatch(r"/dev/input/event\d+", node):
            raise ValueError(f"Use an explicit /dev/input/eventN in XR_INPUT_DEVICES: {node}")
        devices.append((node, node))
    for node in filter(None, (p.strip() for p in env.get("XR_SERIAL_DEVICES", "").split(","))):
        if not re.fullmatch(r"/dev/(?:tty(?:ACM|USB)\d+|serial/by-id/[^/:]+)", node) or Path(node).name in (".", ".."):
            raise ValueError(f"Use /dev/ttyACMN, /dev/ttyUSBN or /dev/serial/by-id/<id> in XR_SERIAL_DEVICES: {node}")
        devices.append((node, node))
    return list(dict.fromkeys(devices))


def hardware_override(env):
    devices = discover(env)
    if env.get("XR_REQUIRE_CAMERA", "1") == "1" and not any(target == "/dev/video0" for _, target in devices):
        raise RuntimeError("No selected UVC camera found. Set XR_USB_IDS or XR_CAMERA_DEVICE, or XR_REQUIRE_CAMERA=0 for camera-free profiles")
    if env.get("XR_REQUIRE_HIDRAW", "0") == "1" and not any("/hidraw" in target for _, target in devices):
        raise RuntimeError("No selected hidraw node found. Check XR_USB_IDS and USB passthrough, or set XR_REQUIRE_HIDRAW=0")
    for source, _ in devices:
        if not Path(source).exists() or not stat.S_ISCHR(Path(source).stat().st_mode):
            raise RuntimeError(f"Device missing or not a character device: {source}")
    service = {"devices": [f"{source}:{target}:rw" for source, target in devices]} if devices else {}
    volumes = []
    if Path("/run/udev").is_dir():
        volumes.append({"type": "bind", "source": "/run/udev", "target": "/run/udev", "read_only": True})
    if env.get("XR_BLUETOOTH", "0") == "1":
        bus = Path("/run/dbus/system_bus_socket")
        if not bus.exists() or not stat.S_ISSOCK(bus.stat().st_mode):
            raise RuntimeError("Host D-Bus socket missing; start dbus/BlueZ on the Linux host")
        if not glob.glob("/sys/class/bluetooth/hci*"):
            raise RuntimeError("No host Bluetooth HCI adapter. Check USB attachment, btusb, firmware and rfkill")
        if not shutil.which("bluetoothctl") or not call(["bluetoothctl", "list"], env, True):
            raise RuntimeError("BlueZ has no controller; start bluetooth.service and check bluetoothctl list")
        volumes.append({"type": "bind", "source": str(bus), "target": str(bus), "read_only": True})
        service["environment"] = {"DBUS_SYSTEM_BUS_ADDRESS": f"unix:path={bus}"}
    if volumes:
        service["volumes"] = volumes
    return {"services": {"runtime": service}}


def compose(env, hardware=False):
    args = ["docker", "compose", "--project-directory", str(ROOT), "-f", str(ROOT / "compose.yaml")]
    if hardware:
        state = ROOT / ".state"
        state.mkdir(mode=0o700, exist_ok=True)
        generated = state / "hardware.compose.json"
        generated.write_text(json.dumps(hardware_override(env), indent=2) + "\n")
        args += ["-f", str(generated)]
    if env.get("XR_VIDEO_TCP", "0") == "1":
        args += ["-f", str(ROOT / "compose.video.yaml")]
    return args


def prepare_paths(env):
    for key, default in (("XR_CONFIG_DIR", "config"), ("XR_LOG_DIR", "logs")):
        path = Path(env.get(key, default)).expanduser()
        if not path.is_absolute():
            path = ROOT / path
        path.mkdir(parents=True, exist_ok=True)
        env[key] = str(path.resolve())


def export_configs(env, destination):
    destination = Path(destination).resolve()
    if destination.exists():
        raise RuntimeError(f"Export destination already exists; refusing to overwrite: {destination}")
    container = call(["docker", "create", "--entrypoint", "/bin/true", image(env)], env, True)
    try:
        destination.mkdir(parents=True)
        call(["docker", "cp", f"{container}:/opt/xr-gate/bin/python/xr_client/configs", destination / "xr_client"], env)
        call(["docker", "cp", f"{container}:/opt/xr-gate/bin/override_controller/configs", destination / "override_controller"], env)
    finally:
        call(["docker", "rm", container], env)
    # Running the helper via sudo should still produce editable host files.
    if os.geteuid() == 0 and os.environ.get("SUDO_UID"):
        uid, gid = int(os.environ["SUDO_UID"]), int(os.environ["SUDO_GID"])
        for path in [destination, *destination.rglob("*")]:
            os.chown(path, uid, gid)
    print(f"Exported unchanged release configs: {destination}")


def doctor(env):
    failed = False
    print(f"Linux kernel: {os.uname().release}; machine: {os.uname().machine}")
    try:
        info = local_engine(env)
        print(f'Docker: {info["ServerVersion"]}, {info.get("OperatingSystem", "")}, local socket')
    except (RuntimeError, subprocess.CalledProcessError) as error:
        print(f"FAIL Docker: {error}")
        failed = True
    try:
        devices = discover(env)
        for source, target in devices:
            print(f"Device: {source} -> {target}")
        override = hardware_override(env)
        print("OK hardware preflight; USB mappings:", len(override["services"]["runtime"]["devices"]))
    except (RuntimeError, ValueError, subprocess.CalledProcessError) as error:
        print(f"FAIL hardware: {error}")
        failed = True
    for command in (["lsusb"], ["bluetoothctl", "list"], ["rfkill", "list"], ["systemctl", "is-active", "bluetooth"]):
        if shutil.which(command[0]):
            call(command, env, check=False)
    print("Kernel modules: sudo modprobe uvcvideo; sudo modprobe hid_generic; sudo modprobe btusb")
    print("After USB reattach: stop the container and start it again to refresh device mappings.")
    return int(failed)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("build", "run", "up", "console", "stop", "logs", "doctor", "config", "export-configs", "check-tcp", "check-image", "shell"))
    parser.add_argument("extra", nargs=argparse.REMAINDER, help="xr_client args after run/up, or export destination")
    args = parser.parse_args()
    extra = args.extra[1:] if args.extra[:1] == ["--"] else args.extra
    env = settings()
    prepare_paths(env)
    if args.command == "doctor":
        return doctor(env)
    if args.command == "check-tcp":
        sys.path.insert(0, str(ROOT / "scripts"))
        from tcp_probe import probe
        probe("127.0.0.1", int(env.get("XR_TCP_PORT", "45810")))
        return 0
    if args.command == "config":
        return call(compose(env) + ["config"], env)
    local_engine(env)
    if args.command == "build":
        return call(compose(env) + ["build", "--pull", "runtime"], env)
    if args.command in ("run", "up"):
        command = compose(env, hardware=True)
        if args.command == "run":
            return call(command + ["run", "--rm", "--service-ports", "--name", NAME, "runtime", *extra], env)
        if extra:
            raise ValueError("For xr_client arguments use run; up uses the configured profile")
        return call(command + ["up", "-d", "--no-build", "runtime"], env)
    if args.command == "console":
        return call(["docker", "attach", "--sig-proxy=false", NAME], env)
    if args.command == "stop":
        call(["docker", "stop", "--time", "20", NAME], env)
        # Foreground run removes itself; background up leaves a stopped container.
        remaining = call(["docker", "container", "inspect", "--format", "{{.Id}}", NAME], env, True, False)
        if remaining:
            call(["docker", "rm", NAME], env)
        return 0
    if args.command == "logs":
        return call(["docker", "logs", "--tail", "100", "-f", NAME], env)
    if args.command == "export-configs":
        export_configs(env, extra[0] if extra else ROOT / "exported-configs")
        return 0
    if args.command == "check-image":
        return call(["docker", "run", "--rm", "--entrypoint", "python3", image(env),
                     "/usr/local/lib/xr-gate/check-runtime.py"], env)
    if args.command == "shell":
        return call(["docker", "exec", "-it", NAME, "bash"], env)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except KeyboardInterrupt:
        raise SystemExit(130)
    except subprocess.CalledProcessError as error:
        if error.stderr:
            print(error.stderr.strip(), file=sys.stderr)
        raise SystemExit(error.returncode)
    except (RuntimeError, ValueError, OSError) as error:
        print(f"xr-gate: {error}", file=sys.stderr)
        raise SystemExit(1)
