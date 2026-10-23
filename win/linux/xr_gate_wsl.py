#!/usr/bin/env python3
"""Linux half of XrGate-Wsl.ps1; standard library only, no Docker."""
from __future__ import annotations

import base64
import configparser
import ctypes
import fcntl
import hashlib
import json
import os
import platform
import pwd
import re
import shutil
import signal
import socket
import subprocess
import sys
import tarfile
import tempfile
import time
import uuid
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


def check_runtime_guard(request):
    guard = request.get("runtime_guard")
    if guard is None:
        return  # Existing CLI sessions do not have a Windows window owner.
    if not isinstance(guard, dict) or not re.fullmatch(r"[a-f0-9]{32}", guard.get("token", "")):
        raise ValueError("Invalid runtime startup guard.")
    record = json.loads(Path(guard["path"]).read_text(encoding="utf-8-sig"))
    if record.get("Token") != guard["token"] or record.get("Stopping") is not False:
        raise RuntimeError("Runtime startup was cancelled by the launcher.")


def run(args, *, env=None, check=True, timeout=None):
    return subprocess.run([str(x) for x in args], env=env, check=check, timeout=timeout)


def run_diagnostic(args):
    command = " ".join(str(x) for x in args)
    log("Checking: " + command)
    try:
        run(args, check=False, timeout=10)
    except subprocess.TimeoutExpired:
        log("WARNING: diagnostic timed out after 10 seconds: " + command)


def decode_request(value):
    data = json.loads(base64.b64decode(value, validate=True).decode("utf-8-sig"))
    if not isinstance(data, dict):
        raise ValueError("Expected a JSON object.")
    user = data.get("user", "")
    if not re.fullmatch(r"[a-z_][a-z0-9_-]*[$]?", user) or user == "root":
        raise ValueError("A regular Linux user is required.")
    return data


def process_identity(pid):
    """Use the boot ID and kernel start time, never just a reusable numeric PID."""
    validate_proc_namespace()
    if type(pid) is not int or pid <= 1:
        raise ValueError("Invalid managed runtime PID.")
    process = Path("/proc") / str(pid)
    fields = (process / "stat").read_text().rsplit(")", 1)[1].split()
    return {"pid": pid, "start_ticks": fields[19],
            "boot_id": Path("/proc/sys/kernel/random/boot_id").read_text().strip(),
            "uid": process.stat().st_uid}


def validate_proc_namespace():
    if int(Path("/proc/self/stat").read_text().split(" ", 1)[0]) != os.getpid():
        raise RuntimeError("/proc belongs to a different PID namespace; refusing unsafe process management.")


def owned_processes(session):
    """Find this session's descendants, including detached service groups."""
    validate_proc_namespace()
    token = session.get("session_id", "")
    if not re.fullmatch(r"[0-9a-f]{32}", token) or session.get("uid") != os.geteuid():
        raise ValueError("Invalid managed session ownership metadata.")
    if session.get("boot_id") != Path("/proc/sys/kernel/random/boot_id").read_text().strip():
        return []
    marker = ("XR_GATE_SESSION_ID=" + token).encode()
    result = []
    for path in Path("/proc").iterdir():
        if not path.name.isdigit():
            continue
        try:
            if path.stat().st_uid != os.geteuid():
                continue
            if marker not in (path / "environ").read_bytes().split(b"\0"):
                continue
            fields = (path / "stat").read_text().rsplit(")", 1)[1].split()
            if fields[0] != "Z":
                result.append(process_identity(int(path.name)))
        except (FileNotFoundError, ProcessLookupError, PermissionError):
            continue
    return result


def signal_identity(identity, signum):
    """Pin the PID before checking start time; never signal a reused PID."""
    try:
        descriptor = os.pidfd_open(identity["pid"])
        try:
            current = process_identity(identity["pid"])
            if any(current.get(key) != identity.get(key) for key in current) or current["uid"] != os.geteuid():
                raise RuntimeError("The recorded PID belongs to a different process; refusing to signal it.")
            signal.pidfd_send_signal(descriptor, signum)
        finally:
            os.close(descriptor)
    except (ProcessLookupError, FileNotFoundError):
        return


def wait_owned(session, seconds):
    deadline = time.monotonic() + seconds
    while owned_processes(session):
        if time.monotonic() >= deadline:
            return False
        time.sleep(0.1)
    return True


def supervise_runtime(args, env, session, graceful_seconds=30):
    """Keep the lock in a supervisor until every owned service has exited."""
    # Adopt orphaned grandchildren instead of leaving zombies in WSL's PID 1.
    if ctypes.CDLL(None, use_errno=True).prctl(36, 1, 0, 0, 0) != 0:  # PR_SET_CHILD_SUBREAPER
        raise OSError(ctypes.get_errno(), "Cannot supervise runtime descendants")
    stopping = False
    def request_stop(_signum, _frame):
        nonlocal stopping
        stopping = True
    previous = {sig: signal.signal(sig, request_stop) for sig in (signal.SIGTERM, signal.SIGHUP, signal.SIGINT)}
    child = None
    try:
        child = subprocess.Popen(args, env=dict(env, XR_GATE_SESSION_ID=session["session_id"]), start_new_session=True)
        child_identity = process_identity(child.pid)
        while child.poll() is None and not stopping:
            time.sleep(0.1)
        if child.poll() is None:
            signal_identity(child_identity, signal.SIGTERM)
            try:
                child.wait(timeout=graceful_seconds)
            except subprocess.TimeoutExpired:
                log("Runtime shutdown exceeded its grace period; stopping the remaining owned services.")
        # xr_client normally cleans its own groups/BLE links. Also handle an
        # exited launcher with surviving children, and services ignoring TERM.
        for identity in owned_processes(session):
            signal_identity(identity, signal.SIGTERM)
        if not wait_owned(session, 3):
            for identity in owned_processes(session):
                signal_identity(identity, signal.SIGKILL)
        if not wait_owned(session, 3):
            raise RuntimeError("Owned runtime processes are still alive; USB must remain attached.")
        if child.poll() is None:
            child.wait(timeout=3)
        while True:
            try:
                if os.waitpid(-1, os.WNOHANG)[0] == 0:
                    break
            except ChildProcessError:
                break
        return child.returncode
    finally:
        for sig, handler in previous.items():
            signal.signal(sig, handler)


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
        if not Path(folder).is_absolute():
            raise ValueError("XR_LINUX_ROOT must be an absolute path or start with ~/.")
        self.base = Path(folder).resolve()
        self.validate_base()
        self.current = self.base / "current"
        self.config = self.base / "config"
        self.lock = self.base / "runtime.lock"

    def root_required(self):
        if os.geteuid() != 0:
            raise RuntimeError("This setup operation requires root inside the selected WSL distro.")
        self.validate_base()

    def validate_base(self):
        if not self.base.is_absolute() or self.base.resolve() == Path("/"):
            raise ValueError("XR_LINUX_ROOT must not resolve to the Linux filesystem root.")

    def user_required(self):
        if os.geteuid() != self.account.pw_uid:
            raise RuntimeError("Start the runtime under the configured regular Linux user.")

    def own(self, path):
        if os.geteuid() == 0:
            os.chown(path, self.account.pw_uid, self.account.pw_gid, follow_symlinks=False)

    def directory(self, path):
        self.validate_base()
        if path.resolve() == Path("/"):
            raise ValueError("Refusing to create or change ownership of the filesystem root.")
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
        kind = "runtime"
        if self.lock.exists():
            with self.lock.open("a+") as handle:
                try:
                    fcntl.flock(handle, fcntl.LOCK_EX | fcntl.LOCK_NB)
                except BlockingIOError:
                    running = True
        session = self.base / "runtime-session.json"
        try:
            saved = json.loads(session.read_text())
            kind = saved.get("session_kind", "runtime")
            if saved.get("session_id"):
                running = running or bool(owned_processes(saved))
        except FileNotFoundError:
            pass
        return {"installed": (self.current / "run_xr_client.sh").is_file(),
                "running": running and kind == "runtime", "busy": running,
                "session_kind": kind if running else "", "root": str(self.current.resolve())}

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
            packages = ["ca-certificates", "python3", "python3-venv", "util-linux", "kmod",
                        "acl", "usbutils", "v4l-utils", "udev", "bluez", "rfkill", "libasound2t64"]
            run(["apt-get", "install", "-y", "--no-install-recommends", *packages], env=env)
            if self.cfg.get("bluetooth") or self.cfg.get("gearvr"):
                self.firmware("install")
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
            for group in ("video", "input", "plugdev", "bluetooth", "audio"):
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
        for node in (sysroot / "class/sound").glob("*"):
            if re.fullmatch(r"(?:controlC\d+|pcmC\d+D\d+p)", node.name) and self.selected_usb(node):
                nodes.append(devroot / "snd" / node.name)
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
        if self.settings.get("XR_AUDIO_OUTPUT", "none") not in ("", "none"):
            run(["modprobe", "snd_usb_audio"], check=False)
        for module in csv_setting(self.settings, "XR_KERNEL_MODULES"):
            if not re.fullmatch(r"[A-Za-z0-9_-]+", module):
                raise ValueError("Invalid kernel module: " + module)
            run(["modprobe", module], check=False)
        if self.cfg.get("bluetooth"):
            run(["systemctl", "start", "bluetooth"])
            run(["rfkill", "unblock", "bluetooth"], check=False)
        run(["udevadm", "control", "--reload-rules"], check=False)
        for subsystem in ("video4linux", "hidraw", "tty", "input", "sound"):
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
            self.wait_for_bluetooth()

    def firmware(self, operation, *, capture=False, timeout=None):
        helper = Path(__file__).with_name("wsl_firmware.py")
        if not helper.is_file():
            raise FileNotFoundError("Extract the complete updated Windows archive, including linux/wsl_firmware.py.")
        request = base64.b64encode(json.dumps(self.cfg).encode()).decode("ascii")
        args = [sys.executable, str(helper), operation, request]
        if capture:
            result = subprocess.run(args, capture_output=True, text=True, timeout=timeout, check=True)
            return json.loads(result.stdout)
        run(args, timeout=timeout)

    def wait_for_bluetooth(self):
        seconds = integer_setting(self.settings, "XR_BLUETOOTH_READY_TIMEOUT", 1, 120)
        deadline = time.monotonic() + seconds
        last_error = "No HCI controller is available."
        while time.monotonic() < deadline:
            try:
                if self.cfg.get("bluetooth_usb_id"):
                    state = self.firmware("status", capture=True, timeout=min(3, max(0.05, deadline - time.monotonic())))
                    if state["ready"]:
                        log("Selected Bluetooth HCI controller is ready: " + state["address"])
                        return
                    last_error = "The selected USB dongle has no ready BlueZ HCI controller."
                    time.sleep(min(0.25, max(0, deadline - time.monotonic())))
                    continue
                result = subprocess.run(["bluetoothctl", "list"], capture_output=True, text=True,
                                        env=dict(os.environ, LC_ALL="C"), check=False,
                                        timeout=min(3, max(0.05, deadline - time.monotonic())))
                if result.returncode == 0 and re.search(r"(?m)^Controller\s+[0-9A-Fa-f:]{17}\s", result.stdout):
                    print(result.stdout.strip(), flush=True)
                    log("Bluetooth HCI controller is ready.")
                    return
                last_error = result.stderr.strip() or "bluetoothctl list returned no HCI controller."
            except subprocess.TimeoutExpired:
                last_error = "bluetoothctl list timed out."
            time.sleep(min(0.25, max(0, deadline - time.monotonic())))
        raise RuntimeError(f"Bluetooth is enabled but no controller became ready in {seconds}s. "
                           f"{last_error} Check the selected USB dongle, kernel module, firmware and bluetooth.service. "
                           "If Bluetooth is not needed, disable it in Controllers (XR_BLUETOOTH=0).")

    def controller_template(self, package):
        name = self.settings.get("XR_GEARVR_TEMPLATE", "")
        if not name or Path(name).name != name:
            raise ValueError("Set XR_GEARVR_TEMPLATE to a controller template filename.")
        return package_path(package, "bin/override_controller/configs/" + name)

    def generate_profile(self):
        custom = self.config / "xr_client.json"
        mode = self.settings.get("XR_CONTROLLER_MODE", "")
        if mode not in ("", "gearvr", "gestures"):
            raise ValueError("XR_CONTROLLER_MODE must be gearvr or gestures.")
        if custom.is_file() and not mode:
            return custom
        package = self.current.resolve()
        if custom.is_file():
            # Apply the selected mode to a copy, never rewrite a user profile.
            data = json.loads(custom.read_text())
            data.setdefault("root_project", str(package))
            data.setdefault("log_dir", str(self.base / "logs"))
        else:
            profile = self.settings.get("XR_PROFILE", "")
            if not re.fullmatch(r"[A-Za-z0-9_.-]+", profile) or profile in (".", ".."):
                raise ValueError("Select XR_PROFILE in the hardware env or provide config/xr_client.json.")
            filename = profile if profile.endswith(".json") else profile + ".json"
            data = json.loads(package_path(package, "bin/python/xr_client/configs/" + filename).read_text())
            data["root_project"] = str(package)
            data["log_dir"] = str(self.base / "logs")
        controllers = self.config / "override_controller.json"
        if mode == "gearvr" and not controllers.is_file():
            self.write(controllers, self.controller_template(package).read_text())
        found_controller = False
        for group in ("pre_gate_services", "post_gate_services", "foreground_services"):
            if group not in data:
                continue  # Preserve xr_client defaults for omitted custom sections.
            if mode == "gestures":
                # Removing these also prevents gesture/manual toggle actions or
                # inherited RUN_OVERRIDE_CONTROLLER=1 from starting the publisher.
                data[group] = [service for service in data.get(group, [])
                               if service.get("name") not in ("override_controller", "override_controller_service")]
            for service in data.get(group, []):
                if service.get("name") == "override_controller":
                    found_controller = True
                    service["start_on_launch"] = True if mode == "gearvr" else (self.config / "controllers-ready").is_file()
                    if mode == "gearvr":
                        service["enabled"] = True
                        service["enable_env"] = "XR_GATE_CONTROLLER_INPUT"
                    service.setdefault("env", {}).update({
                        "CONFIG_PATH": str(controllers),
                        "PROVIDERS": self.settings.get("XR_GEARVR_PROVIDERS" if mode == "gearvr" or self.cfg.get("gearvr") else "XR_CONTROLLER_PROVIDERS", ""),
                    })
        if mode == "gearvr" and not found_controller:
            raise ValueError("The selected xr_client profile has no override_controller service for Gear VR Controllers.")
        path = self.base / "generated/profile.json"
        self.write(path, json.dumps(data, indent=2) + "\n")
        return path

    def start(self):
        self.user_required()
        if not self.status()["installed"]:
            raise RuntimeError("Run -Action Install first.")
        with self.acquire_lock() as handle:
            self._start_locked(handle)

    def _start_locked(self, handle):
        # Lock before checking Stop's token, preventing late launches after detach.
        check_runtime_guard(self.cfg)
        try:
            host = self.settings.get("XR_TCP_HOST", "127.0.0.1")
            port = integer_setting(self.settings, "XR_TCP_PORT")
            with socket.socket() as probe:
                probe.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
                probe.bind((host, port))
        except OSError as exc:
            raise RuntimeError("The configured TCP port is occupied. Stop the other runtime before starting this one.") from exc
        # Only the supervisor owns this lock. Child lifetime is checked separately.
        os.set_inheritable(handle.fileno(), False)
        package = self.current.resolve()
        option = self.cfg.get("display_option", "current")
        if option not in ("current", "60hz", "90hz", "prompt"):
            raise ValueError("Invalid display option.")
        profile = self.generate_profile()
        env = dict(os.environ, AUTO_BUILD="0", RUNTIME_APP_TCP="1",
                   RUNTIME_APP_TCP_HOST=host, RUNTIME_APP_TCP_PORT=str(port),
                   ROOT_PROJECT=str(package), XR_ROOT_PROJECT=str(package))
        if self.settings.get("XR_CONTROLLER_MODE") == "gearvr":
            env["XR_GATE_CONTROLLER_INPUT"] = "1"
        elif self.settings.get("XR_CONTROLLER_MODE") == "gestures":
            # Omitted custom sections inherit xr_client defaults. Disable their
            # publishers too, while retaining the default capture/hand services.
            env.update(RUN_OVERRIDE_CONTROLLER="0", RUN_OVERRIDE_CONTROLLER_SERVICE="0")
        if self.settings.get("XR_HMD_MAX_AGE_MS"):
            age_ms = integer_setting(self.settings, "XR_HMD_MAX_AGE_MS", maximum=5000)
            env["MAX_HMD_AGE_MS"] = str(age_ms)
            log(f"HMD source max age: {age_ms} ms (this does not reduce capture latency).")
        args = ["bash", str(package / "run_xr_client.sh"), "--config", str(profile),
                "--root", str(package), "--prestart-option", option]
        os.chdir(package)
        log(f"Application TCP: {host}:{port}")
        log("Stop with Ctrl+C in this console. Startup gate and calibration stay enabled.")
        check_runtime_guard(self.cfg)
        self.run_managed(args, env, handle=handle)

    def run_managed(self, args, env, *, handle=None, kind="runtime"):
        if handle is None:
            handle = self.acquire_lock()
        session = dict(process_identity(os.getpid()), session_id=uuid.uuid4().hex, session_kind=kind)
        session_path = self.base / "runtime-session.json"
        self.write(session_path, json.dumps(session) + "\n")
        try:
            code = supervise_runtime(args, env, session, graceful_seconds=30 if kind == "runtime" else 2)
            if code and (kind != "runtime" or code not in (-signal.SIGTERM, -signal.SIGINT, 130, 143)):
                raise RuntimeError("xr_client exited with code " + str(code))
            return code
        finally:
            # If cleanup failed, keep metadata so Stop can recover the exact
            # owned descendants even after this supervisor exits.
            if not owned_processes(session):
                session_path.unlink(missing_ok=True)
            handle.close()

    def stop(self):
        self.user_required()
        session = self.base / "runtime-session.json"
        status = self.status()
        if not status.get("busy", status["running"]):
            log("The managed XR Gate runtime is already stopped.")
            return
        if not session.is_file():
            # A newly acquired runtime lock can precede session metadata while
            # the profile is being generated. Do not mistake it for an old build.
            deadline = time.monotonic() + 5
            while self._lock_held() and not session.is_file() and time.monotonic() < deadline:
                time.sleep(0.05)
            if not self._lock_held() and not session.is_file():
                log("Runtime startup was cancelled before services were launched.")
                return
        if not session.is_file():
            raise RuntimeError("This runtime was started by an older launcher. Stop it with Ctrl+C in its console.")
        saved = json.loads(session.read_text())
        if saved.get("session_id"):
            owned_processes(saved)  # Validate ownership before signalling anything.
            log("Stopping the managed runtime and waiting for all owned services...")
            try:
                signal_identity(saved, signal.SIGTERM)
            except RuntimeError:
                # A dead supervisor's PID may have been reused. The unique
                # descendant token remains authoritative for orphan recovery.
                log("The supervisor has exited; recovering its owned descendants.")
            if not self._lock_held():
                for identity in owned_processes(saved):
                    signal_identity(identity, signal.SIGTERM)
            deadline = time.monotonic() + 40
            while self.status().get("busy", False):
                if time.monotonic() >= deadline:
                    for identity in owned_processes(saved):
                        signal_identity(identity, signal.SIGKILL)
                    if not wait_owned(saved, 3) or self.status().get("busy", False):
                        raise RuntimeError("Runtime cleanup is incomplete. USB remains attached; retry Stop.")
                    break
                time.sleep(0.1)
            if session.is_file() and json.loads(session.read_text()) == saved:
                session.unlink()
            log("All managed runtime services stopped.")
            return
        pid = saved.get("pid")
        try:
            descriptor = os.pidfd_open(pid)
        except ProcessLookupError:
            raise RuntimeError("The recorded runtime exited but its lock is still held. Check the tracking console.") from None
        try:
            if saved != process_identity(pid) or saved["uid"] != os.geteuid():
                raise RuntimeError("The recorded PID belongs to a different process. Refusing to signal it.")
            log("Stopping the managed runtime and waiting for service/BLE cleanup...")
            signal.pidfd_send_signal(descriptor, signal.SIGTERM)
        finally:
            os.close(descriptor)
        deadline = time.monotonic() + 30
        while self.status()["running"]:
            if time.monotonic() >= deadline:
                raise RuntimeError("Runtime cleanup did not finish in 30 seconds. USB remains attached; check the tracking console.")
            time.sleep(0.1)
        # A successful stop releases the lock before Windows returns USB devices.
        if session.is_file() and json.loads(session.read_text()) == saved:
            session.unlink()
        log("Managed runtime stopped.")

    def _lock_held(self):
        if not self.lock.exists():
            return False
        with self.lock.open("a+") as handle:
            try:
                fcntl.flock(handle, fcntl.LOCK_EX | fcntl.LOCK_NB)
                return False
            except BlockingIOError:
                return True

    def bluetooth_controllers(self, scan=False):
        self.user_required()
        if self.status().get("busy", False):
            raise RuntimeError("Stop tracking before selecting or pairing controllers.")
        package = self.current.resolve()
        helper = package / "bin/scripts/override_controller/bluetooth_controllers.py"
        if not helper.is_file():
            helper = Path(__file__).with_name("bluetooth_controllers.py")
        if not helper.is_file():
            helper = Path(__file__).resolve().parents[2] / "override_controller/scripts/linux/bluetooth_controllers.py"
        if not helper.is_file():
            raise FileNotFoundError("Bluetooth controller helper is missing; extract the complete Windows package.")
        destination = self.config / "override_controller.json"
        args = [sys.executable, str(helper), "--config", str(destination), "--template", str(self.controller_template(package))]
        if scan:
            args += ["--scan", "--scan-seconds", str(integer_setting(self.settings, "XR_BLUETOOTH_SCAN_SECONDS", maximum=30))]
        elif "left_controller" in self.cfg or "right_controller" in self.cfg:
            args += ["--left", self.cfg.get("left_controller") or "none", "--right", self.cfg.get("right_controller") or "none"]
        self.run_managed(args, dict(os.environ), kind="bluetooth_scan" if scan else "bluetooth_pair")
        if not scan:
            self.write(self.config / "controllers-ready", "bluetooth-selected\n")

    def controllers(self):
        self.user_required()
        if self.status().get("busy", False):
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
                   GEARVR_INITIAL_SCAN_MS=str(integer_setting(self.settings, "XR_CONTROLLER_INITIAL_SCAN_MS", 1, 600000)))
        self.run_managed(args, env, kind="controllers")
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
                run_diagnostic(cmd)
        for pattern in ("video*", "hidraw*"):
            for path in sorted(Path("/dev").glob(pattern)):
                log(str(path) + " readable=" + str(os.access(path, os.R_OK)))
        if (self.current / "devices/common/linux/scripts/runtime/cpu_target.py").is_file():
            run_diagnostic(["python3", self.current / "devices/common/linux/scripts/runtime/cpu_target.py",
                            "check", self.current])


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
    elif action == "firmware":
        runtime.root_required()
        runtime.firmware("prepare")
    elif action == "run":
        runtime.start()
    elif action == "stop":
        runtime.stop()
    elif action == "controllers":
        runtime.controllers()
    elif action == "bluetooth_scan":
        runtime.bluetooth_controllers(scan=True)
    elif action == "pair":
        runtime.bluetooth_controllers()
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

