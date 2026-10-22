#!/usr/bin/env python3
"""Linux half of XrGate-Wsl.ps1; standard library only, no Docker."""
from __future__ import annotations

import base64
import configparser
import fcntl
import hashlib
import json
import os
import platform
import pwd
import re
import shutil
import socket
import subprocess
import sys
import tarfile
import tempfile
import time
from datetime import datetime
from pathlib import Path, PurePosixPath

def request_settings(request):
    values = request.get("settings", {})
    if not isinstance(values, dict) or any(not isinstance(k, str) or not isinstance(v, str) for k, v in values.items()):
        raise ValueError("Settings must contain literal string values.")
    return values


def integer_setting(settings, key, minimum=1, maximum=65535):
    value = int(settings.get(key, ""))
    if not minimum <= value <= maximum:
        raise ValueError(f"{key} must be from {minimum} to {maximum}.")
    return value


def csv_setting(settings, key):
    return [part.strip() for part in settings.get(key, "").split(",") if part.strip()]


def package_path(package, relative):
    path = (package / relative).resolve()
    if not relative or path == package.resolve() or package.resolve() not in path.parents:
        raise ValueError("Expected a path inside the runtime package: " + relative)
    return path


def log(message):
    print("[XR Gate WSL] " + message, flush=True)


def run(args, *, env=None, check=True):
    return subprocess.run([str(x) for x in args], env=env, check=check)


def decode_request(value):
    data = json.loads(base64.b64decode(value, validate=True).decode("utf-8-sig"))
    if not isinstance(data, dict):
        raise ValueError("Expected a JSON object.")
    user = data.get("user", "")
    if not re.fullmatch(r"[a-z_][a-z0-9_-]*[$]?", user) or user == "root":
        raise ValueError("A regular Linux user is required.")
    return data


def configure_windows_display(request):
    """Update driver defaults and SteamVR overrides without folding JSON keys."""
    x, y, hz = (request.get(key) for key in ("window_x", "window_y", "frequency_hz"))
    if any(type(value) is not int for value in (x, y, hz)) or not 1 <= hz <= 1000:
        raise ValueError("Display coordinates must be integers; frequency must be from 1 to 1000 Hz.")
    mode = request.get("display_mode", "extended_sbs")
    if mode not in ("extended_sbs", "direct"):
        raise ValueError("Invalid display mode.")
    driver = Path(request["driver_settings"])
    steam = Path(request["steamvr_settings"])
    if not driver.is_absolute() or not steam.is_absolute() or driver == steam:
        raise ValueError("Two distinct absolute display-settings paths are required.")
    if not driver.is_file():
        raise FileNotFoundError("Install the Windows driver before selecting its display.")
    settings = request_settings(request)
    geometry = {"windowX": x, "windowY": y, "displayFrequency": hz}
    for field, key in (("windowWidth", "XR_WINDOW_WIDTH"), ("windowHeight", "XR_WINDOW_HEIGHT"),
                       ("renderWidth", "XR_RENDER_WIDTH"), ("renderHeight", "XR_RENDER_HEIGHT")):
        geometry[field] = integer_setting(settings, key)

    xr_values = dict(geometry, runtimeTcpHost=settings.get("XR_TCP_HOST", "127.0.0.1"),
                     runtimeTcpPort=integer_setting(settings, "XR_TCP_PORT"), isDisplayOnDesktop=(mode == "extended_sbs"), isDisplayRealDisplay=True,
                     displayDebugMode=False)
    steam_values = dict(geometry, directMode=(mode == "direct"), displayDebugMode=False,
                        debugMode=False, DebugMode=False)
    for field, key in (("ipdMeters", "XR_IPD_METERS"), ("projectionLeft", "XR_PROJECTION_LEFT"),
                       ("projectionRight", "XR_PROJECTION_RIGHT"), ("projectionTop", "XR_PROJECTION_TOP"),
                       ("projectionBottom", "XR_PROJECTION_BOTTOM")):
        if settings.get(key):
            value = float(settings[key])
            if not (-100 < value < 100) or (field == "ipdMeters" and not 0.01 <= value <= 0.2):
                raise ValueError("Invalid optical setting: " + key)
            xr_values[field] = value
    for field, key in (("serialNumber", "XR_HMD_SERIAL"), ("leftHandSerial", "XR_LEFT_HAND_SERIAL"),
                       ("rightHandSerial", "XR_RIGHT_HAND_SERIAL")):
        if settings.get(key):
            xr_values[field] = settings[key]
    changes = []
    # Parse and validate both files before writing either one. json preserves
    # distinct keys such as debugMode/DebugMode, unlike PowerShell 5.1 objects.
    for path in (driver, steam):
        original = path.read_bytes() if path.exists() else None
        data = json.loads(original.decode("utf-8-sig")) if original is not None else {}
        if not isinstance(data, dict):
            raise ValueError("Settings must contain a JSON object: " + str(path))
        for section, values in (("xr_tracking", xr_values), ("steamvr", steam_values)):
            obj = data.setdefault(section, {})
            if not isinstance(obj, dict):
                raise ValueError("Invalid settings section " + section + ": " + str(path))
            obj.update(values)
        # Keep bytes and timestamps untouched if all requested values already match.
        if original is not None and json.loads(original.decode("utf-8-sig")) == data:
            continue
        changes.append((path, original, (json.dumps(data, indent=2, ensure_ascii=False) + "\n").encode("utf-8")))

    stamp = datetime.now().strftime("%Y%m%d-%H%M%S-%f")
    for path, original, _ in changes:
        if original is not None:
            backup = path.with_name(path.name + ".before-display-" + stamp)
            backup.write_bytes(original)
            log("Settings backup: " + str(backup))

    def replace_bytes(path, content):
        temporary = None
        try:
            with tempfile.NamedTemporaryFile(dir=path.parent, prefix=path.name + ".new-", delete=False) as handle:
                temporary = Path(handle.name)
                handle.write(content)
            os.replace(temporary, path)
        finally:
            if temporary is not None:
                temporary.unlink(missing_ok=True)

    written = []
    try:
        for path, original, updated in changes:
            replace_bytes(path, updated)
            written.append((path, original))
    except Exception:
        for path, original in reversed(written):
            if original is None:
                path.unlink(missing_ok=True)
            else:
                replace_bytes(path, original)
        raise
    log(f"Display configured: {mode}, {geometry['windowWidth']}x{geometry['windowHeight']} @ {hz} Hz, X={x}, Y={y}.")


class Runtime:
    def __init__(self, request):
        self.cfg = request
        self.settings = request_settings(request)
        self.account = pwd.getpwnam(request["user"])
        if self.account.pw_uid == 0:
            raise ValueError("Refusing to run XR Gate as root.")
        self.user = self.account.pw_name
        folder = self.settings.get("XR_LINUX_ROOT", "~/xr-gate-wsl")
        if folder.startswith("~/"):
            folder = str(Path(self.account.pw_dir) / folder[2:])
        self.base = Path(folder)
        if not self.base.is_absolute() or self.base == Path("/"):
            raise ValueError("XR_LINUX_ROOT must be an absolute path or start with ~/.")
        self.current = self.base / "current"
        self.config = self.base / "config"
        self.lock = self.base / "runtime.lock"

    def root_required(self):
        if os.geteuid() != 0:
            raise RuntimeError("This setup operation requires root inside the selected WSL distro.")

    def user_required(self):
        if os.geteuid() != self.account.pw_uid:
            raise RuntimeError("Start the runtime under the configured regular Linux user.")

    def own(self, path):
        if os.geteuid() == 0:
            os.chown(path, self.account.pw_uid, self.account.pw_gid, follow_symlinks=False)

    def directory(self, path):
        path.mkdir(parents=True, exist_ok=True)
        self.own(path)

    def write(self, path, text):
        self.directory(path.parent)
        temporary = path.with_name(path.name + ".new")
        temporary.write_text(text, encoding="utf-8")
        self.own(temporary)
        os.replace(temporary, path)

    def as_user(self, args, **kwargs):
        if os.geteuid() == 0:
            return run(["runuser", "-u", self.user, "--", *args], **kwargs)
        self.user_required()
        return run(args, **kwargs)

    def acquire_lock(self):
        self.directory(self.base)
        handle = self.lock.open("a+")
        self.own(self.lock)
        try:
            fcntl.flock(handle, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            handle.close()
            raise RuntimeError("This XR Gate session is already running. Stop it with Ctrl+C first.") from None
        return handle

    def status(self):
        running = False
        if self.lock.exists():
            with self.lock.open("a+") as handle:
                try:
                    fcntl.flock(handle, fcntl.LOCK_EX | fcntl.LOCK_NB)
                except BlockingIOError:
                    running = True
        return {"installed": (self.current / "run_xr_client.sh").is_file(),
                "running": running, "root": str(self.current.resolve())}

    @staticmethod
    def ensure_systemd(path=Path("/etc/wsl.conf"), init_name=None):
        init_name = init_name or Path("/proc/1/comm").read_text().strip()
        if init_name == "systemd":
            return True
        original = path.read_text(encoding="utf-8") if path.exists() else ""
        parser = configparser.ConfigParser(strict=False, interpolation=None)
        parser.read_string(original)
        if parser.get("boot", "systemd", fallback="").lower() != "true":
            if original:
                backup = path.with_name(path.name + ".before-xrgate-" + datetime.now().strftime("%Y%m%d-%H%M%S-%f"))
                shutil.copy2(path, backup)
            if not parser.has_section("boot"):
                parser.add_section("boot")
            parser.set("boot", "systemd", "true")
            with path.open("w", encoding="utf-8") as stream:
                parser.write(stream)
        log("systemd is not running. Save work in this distro; terminate it from Windows, then rerun Install.")
        return False

    @staticmethod
    def verify_archive(path, digest, top_directory):
        if not re.fullmatch(r"[a-fA-F0-9]{64}", digest):
            raise ValueError("Invalid SHA-256.")
        checksum = hashlib.sha256()
        with path.open("rb") as stream:
            for block in iter(lambda: stream.read(1024 * 1024), b""):
                checksum.update(block)
        if checksum.hexdigest() != digest.lower():
            raise ValueError("Archive SHA-256 mismatch: " + str(path))
        with tarfile.open(path, "r:gz") as archive:
            for member in archive:
                p = PurePosixPath(member.name)
                if p.is_absolute() or ".." in p.parts or not p.parts or p.parts[0] != top_directory:
                    raise ValueError("Unexpected archive member: " + member.name)
                if member.isdev() or member.isfifo():
                    raise ValueError("Device/FIFO in runtime archive.")
                if member.issym():
                    target = os.path.normpath(str(p.parent / member.linkname))
                    if PurePosixPath(member.linkname).is_absolute() or not (
                        target == top_directory or target.startswith(top_directory + "/")
                    ):
                        raise ValueError("Archive symlink leaves package.")
                if member.islnk():
                    t = PurePosixPath(member.linkname)
                    if t.is_absolute() or ".." in t.parts or not t.parts or t.parts[0] != top_directory:
                        raise ValueError("Archive hardlink leaves package.")

    def install(self):
        self.root_required()
        release = self.cfg.get("tag", "")
        if not re.fullmatch(r"[A-Za-z0-9._-]+", release):
            raise ValueError("Invalid release tag.")
        distro = platform.freedesktop_os_release()
        if distro.get("ID") != "ubuntu" or distro.get("VERSION_ID") != "24.04":
            raise RuntimeError("This automation requires Ubuntu 24.04. Use a matching WSL distro.")
        if platform.machine() != "x86_64":
            raise RuntimeError("An x86-64 Linux runtime is required.")
        if not self.ensure_systemd():
            return 20
        self.directory(self.base)
        with self.acquire_lock():
            env = dict(os.environ, DEBIAN_FRONTEND="noninteractive")
            run(["apt-get", "update"], env=env)
            packages = ["ca-certificates", "python3", "python3-venv", "util-linux",
                        "acl", "usbutils", "v4l-utils", "udev", "bluez", "rfkill"]
            if self.cfg.get("bluetooth") or self.cfg.get("gearvr"):
                packages.append("linux-firmware")
            run(["apt-get", "install", "-y", "--no-install-recommends", *packages], env=env)
            runtime_archive = Path(self.cfg["runtime_archive"])
            models_archive = Path(self.cfg["models_archive"])
            runtime_hash = self.cfg["runtime_sha256"].lower()
            models_hash = self.cfg["models_sha256"].lower()
            self.verify_archive(runtime_archive, runtime_hash, "xr-gate")
            self.verify_archive(models_archive, models_hash, "hand-tracking-models")
            releases = self.base / "releases"
            self.directory(releases)
            version = releases / (release + "-" + runtime_hash[:12] + "-" + models_hash[:8])
            package = version / "xr-gate"
            if not package.is_dir():
                stage = Path(tempfile.mkdtemp(prefix=".install-", dir=releases))
                self.own(stage)
                try:
                    self.as_user(["tar", "--no-same-owner", "-xzf", runtime_archive, "-C", stage])
                    self.as_user(["tar", "--no-same-owner", "-xzf", models_archive, "-C", stage / "xr-gate/bin"])
                    os.replace(stage, version)
                except BaseException:
                    shutil.rmtree(stage, ignore_errors=True)
                    raise
            installer = package / "devices/common/linux/scripts/runtime/install_runtime_deps_ubuntu.sh"
            hook_name = self.settings.get("XR_DEVICE_ACCESS_SCRIPT", "")
            if not installer.is_file() or not (package / "run_xr_client.sh").is_file():
                raise RuntimeError("The downloaded runtime has an unsupported package layout.")
            env.update(XR_PACKAGE_ROOT=str(package), SUDO_USER=self.user,
                       XR_TARGET_DEVICE=self.settings.get("XR_TARGET_DEVICE", "generic"))
            installer_args = ["bash", installer]
            if hook_name:
                hook = package_path(package, hook_name)
                if not hook.is_file():
                    raise FileNotFoundError("Device access script not found: " + str(hook))
                env["XR_RUNTIME_DEVICE_ACCESS_SCRIPT"] = str(hook)
            else:
                installer_args.append("--no-device-access")
            run(installer_args, env=env)
            for group in ("video", "input", "plugdev", "bluetooth"):
                run(["groupadd", "-f", group])
                run(["usermod", "-aG", group, self.user])
            # The upstream dependency installer creates the thin venv as root.
            # Only this managed runtime tree is handed back to the runtime user.
            for root, dirs, files in os.walk(package, followlinks=False):
                self.own(Path(root))
                for name in dirs + files:
                    self.own(Path(root) / name)
            self.directory(self.config)
            destination = self.config / "override_controller.json"
            imported = self.cfg.get("controller_import")
            if imported:
                payload = Path(imported).read_text(encoding="utf-8-sig")
                if not isinstance(json.loads(payload), dict):
                    raise ValueError("Controller config must be a JSON object.")
                if destination.exists() and destination.read_text() != payload:
                    backup = destination.with_name("override_controller.before-" + datetime.now().strftime("%Y%m%d-%H%M%S-%f") + ".json")
                    shutil.copy2(destination, backup)
                    self.own(backup)
                self.write(destination, payload)
                self.write(self.config / "controllers-ready", "imported\n")
            elif self.cfg.get("gearvr") and not destination.exists():
                template = self.controller_template(package)
                self.write(destination, template.read_text())
            self.write(version / "installed.json", json.dumps({
                "release": release, "runtime_sha256": runtime_hash, "models_sha256": models_hash
            }, indent=2) + "\n")
            link = self.base / ".current-new"
            link.unlink(missing_ok=True)
            link.symlink_to(package, target_is_directory=True)
            os.replace(link, self.current)
            self.directory(self.base / "logs")
            self.directory(self.base / "generated")
            log("Installed: " + str(package))
            log("Shared controller settings: " + str(self.config))
        return 0

    def usb_ids(self):
        identities = {value.lower() for value in csv_setting(self.settings, "XR_USB_IDS")}
        if any(not re.fullmatch(r"[0-9a-f]{4}:[0-9a-f]{4}", value) for value in identities):
            raise ValueError("XR_USB_IDS must contain comma-separated VID:PID pairs.")
        return identities

    def selected_usb(self, path):
        resolved = path.resolve()
        for parent in (resolved, *resolved.parents):
            try:
                identity = (parent / "idVendor").read_text().strip() + ":" + (parent / "idProduct").read_text().strip()
                if identity.lower() in self.usb_ids():
                    return True
            except OSError:
                pass
        return False

    def device_nodes(self, sysroot=Path("/sys"), devroot=Path("/dev")):
        nodes, cameras, hids = [], [], []
        requested = self.settings.get("XR_CAMERA_DEVICE", "")
        for node in (sysroot / "class/video4linux").glob("*"):
            path = devroot / node.name
            if (self.selected_usb(node) and not requested) or (requested and path.resolve() == Path(requested).resolve()):
                cameras.append(path)
        for node in (sysroot / "class/hidraw").glob("*"):
            if self.selected_usb(node):
                hids.append(devroot / node.name)
        for key, pattern in (("XR_HIDRAW_DEVICES", r"/dev/hidraw[0-9]+"),
                             ("XR_SERIAL_DEVICES", r"/dev/(?:tty(?:ACM|USB)[0-9]+|serial/by-id/[^/]+)"),
                             ("XR_INPUT_DEVICES", r"/dev/input/event[0-9]+")):
            for value in csv_setting(self.settings, key):
                if not re.fullmatch(pattern, value) or Path(value).name in (".", ".."):
                    raise ValueError("Invalid device path in " + key + ": " + value)
                (hids if key == "XR_HIDRAW_DEVICES" else nodes).append(Path(value))
        for node in (sysroot / "class/tty").glob("*"):
            if re.fullmatch(r"tty(?:ACM|USB)[0-9]+", node.name) and self.selected_usb(node):
                nodes.append(devroot / node.name)
        for device in (sysroot / "bus/usb/devices").glob("*"):
            if self.selected_usb(device):
                try:
                    bus = int((device / "busnum").read_text())
                    number = int((device / "devnum").read_text())
                    nodes.append(devroot / f"bus/usb/{bus:03d}/{number:03d}")
                except (OSError, ValueError):
                    pass
        return list(dict.fromkeys(cameras + hids + nodes)), cameras, hids

    def prepare(self):
        self.root_required()
        for module in csv_setting(self.settings, "XR_KERNEL_MODULES"):
            if not re.fullmatch(r"[A-Za-z0-9_-]+", module):
                raise ValueError("Invalid kernel module: " + module)
            run(["modprobe", module], check=False)
        if self.cfg.get("bluetooth"):
            run(["systemctl", "start", "bluetooth"])
            run(["rfkill", "unblock", "bluetooth"], check=False)
        run(["udevadm", "control", "--reload-rules"], check=False)
        for subsystem in ("video4linux", "hidraw", "tty", "input"):
            run(["udevadm", "trigger", "--subsystem-match=" + subsystem], check=False)
        deadline = time.monotonic() + 8
        while True:
            nodes, cameras, hids = self.device_nodes()
            missing_camera = self.settings.get("XR_REQUIRE_CAMERA", "0") == "1" and not cameras
            missing_hid = self.settings.get("XR_REQUIRE_HIDRAW", "0") == "1" and not hids
            if not (missing_camera or missing_hid):
                break
            if time.monotonic() >= deadline:
                raise RuntimeError("Selected camera/HID was not found. Check the hardware env, USB attach and -Action Doctor.")
            time.sleep(0.25)
        for node in nodes:
            if not node.is_char_device():
                raise RuntimeError("Device is missing or is not a character device: " + str(node))
            run(["setfacl", "-m", f"u:{self.user}:rw", node])
        log("Selected USB/input device access granted to " + self.user)
        if self.cfg.get("bluetooth"):
            run(["bluetoothctl", "list"])

    def controller_template(self, package):
        name = self.settings.get("XR_GEARVR_TEMPLATE", "")
        if not name or Path(name).name != name:
            raise ValueError("Set XR_GEARVR_TEMPLATE to a controller template filename.")
        return package_path(package, "bin/override_controller/configs/" + name)

    def generate_profile(self):
        custom = self.config / "xr_client.json"
        if custom.is_file():
            return custom
        package = self.current.resolve()
        profile = self.settings.get("XR_PROFILE", "")
        if not re.fullmatch(r"[A-Za-z0-9_.-]+", profile) or profile in (".", ".."):
            raise ValueError("Select XR_PROFILE in the hardware env or provide config/xr_client.json.")
        filename = profile if profile.endswith(".json") else profile + ".json"
        data = json.loads(package_path(package, "bin/python/xr_client/configs/" + filename).read_text())
        data["root_project"] = str(package)
        data["log_dir"] = str(self.base / "logs")
        controllers = self.config / "override_controller.json"
        for service in data.get("post_gate_services", []):
            if service.get("name") == "override_controller":
                service["start_on_launch"] = (self.config / "controllers-ready").is_file()
                service.setdefault("env", {}).update({
                    "CONFIG_PATH": str(controllers),
                    "PROVIDERS": self.settings.get("XR_GEARVR_PROVIDERS" if self.cfg.get("gearvr") else "XR_CONTROLLER_PROVIDERS", ""),
                })
        path = self.base / "generated/profile.json"
        self.write(path, json.dumps(data, indent=2) + "\n")
        return path

    def start(self):
        self.user_required()
        if not self.status()["installed"]:
            raise RuntimeError("Run -Action Install first.")
        handle = self.acquire_lock()
        try:
            host = self.settings.get("XR_TCP_HOST", "127.0.0.1")
            port = integer_setting(self.settings, "XR_TCP_PORT")
            with socket.socket() as probe:
                probe.bind((host, port))
        except OSError as exc:
            raise RuntimeError("The configured TCP port is occupied. Stop the other runtime before starting this one.") from exc
        os.set_inheritable(handle.fileno(), True)
        # Keep the lock alive when Python replaces itself with the shell launcher.
        self._session_lock = handle
        package = self.current.resolve()
        option = self.cfg.get("display_option", "current")
        if option not in ("current", "60hz", "90hz", "prompt"):
            raise ValueError("Invalid display option.")
        profile = self.generate_profile()
        env = dict(os.environ, AUTO_BUILD="0", RUNTIME_APP_TCP="1",
                   RUNTIME_APP_TCP_HOST=host, RUNTIME_APP_TCP_PORT=str(port),
                   ROOT_PROJECT=str(package), XR_ROOT_PROJECT=str(package))
        if self.settings.get("XR_HMD_MAX_AGE_MS"):
            age_ms = integer_setting(self.settings, "XR_HMD_MAX_AGE_MS", maximum=5000)
            env["MAX_HMD_AGE_MS"] = str(age_ms)
            log(f"HMD source max age: {age_ms} ms (this does not reduce capture latency).")
        args = ["bash", str(package / "run_xr_client.sh"), "--config", str(profile),
                "--root", str(package), "--prestart-option", option]
        os.chdir(package)
        log(f"Application TCP: {host}:{port}")
        log("Stop with Ctrl+C in this console. Startup gate and calibration stay enabled.")
        os.execvpe(args[0], args, env)

    def controllers(self):
        self.user_required()
        if self.status()["running"]:
            raise RuntimeError("Stop xr_client before registering controllers.")
        destination = self.config / "override_controller.json"
        package = self.current.resolve()
        if self.cfg.get("gearvr") and not destination.exists():
            self.write(destination, (self.controller_template(package)).read_text())
        args = [str(package / "devices/common/linux/scripts/override_controller/start_override_controller.sh")]
        if self.cfg.get("gearvr"):
            args.append("--connect-devices")
        env = dict(os.environ, ROOT_PROJECT=str(package), XR_ROOT_PROJECT=str(package),
                   CONFIG_PATH=str(destination), PROVIDERS=self.settings.get("XR_GEARVR_CONNECT_PROVIDERS" if self.cfg.get("gearvr") else "XR_CONTROLLER_PROVIDERS", ""),
                   GEARVR_INITIAL_SCAN_MS="10000")
        run(args, env=env)
        if not destination.is_file():
            raise RuntimeError("Controller config was not saved; registration is incomplete.")
        self.write(self.config / "controllers-ready", "registered\n")
        log("Controller config saved; override_controller will start on the next Run.")

    def doctor(self):
        print(json.dumps(self.status(), indent=2), flush=True)
        log("Kernel: " + platform.release())
        log("PID 1: " + Path("/proc/1/comm").read_text().strip())
        for cmd in (["lsusb"], ["bluetoothctl", "list"], ["rfkill", "list"],
                    ["systemctl", "--no-pager", "status", "bluetooth"]):
            if shutil.which(cmd[0]):
                run(cmd, check=False)
        for pattern in ("video*", "hidraw*"):
            for path in sorted(Path("/dev").glob(pattern)):
                log(str(path) + " readable=" + str(os.access(path, os.R_OK)))
        if (self.current / "devices/common/linux/scripts/runtime/cpu_target.py").is_file():
            run(["python3", self.current / "devices/common/linux/scripts/runtime/cpu_target.py",
                 "check", self.current], check=False)


def main(argv):
    if len(argv) != 3:
        raise ValueError("Usage: xr_gate_wsl.py OPERATION BASE64_JSON")
    runtime = Runtime(decode_request(argv[2]))
    action = argv[1]
    if action == "status":
        print(json.dumps(runtime.status()))
    elif action == "install":
        return runtime.install()
    elif action == "prepare":
        runtime.prepare()
    elif action == "run":
        runtime.start()
    elif action == "controllers":
        runtime.controllers()
    elif action == "pair":
        runtime.root_required()
        os.execvp("bluetoothctl", ["bluetoothctl"])
    elif action == "doctor":
        runtime.doctor()
    elif action == "display":
        runtime.user_required()
        configure_windows_display(runtime.cfg)
    else:
        raise ValueError("Unknown operation: " + action)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main(sys.argv))
    except KeyboardInterrupt:
        raise SystemExit(130)
    except Exception as exc:
        print("[XR Gate WSL] ERROR: " + str(exc), file=sys.stderr, flush=True)
        raise SystemExit(1)

