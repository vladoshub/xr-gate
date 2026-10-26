#!/usr/bin/env python3
"""Expose distro firmware to the shared WSL kernel; no hardware IDs built in."""
from __future__ import annotations

import base64
from contextlib import contextmanager
import fcntl
import gzip
import json
import lzma
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import subprocess
import sys
import tempfile


DEFAULTS = {
    "XR_WSL_FIRMWARE": "1",
    "XR_WSL_FIRMWARE_SOURCE": "/lib/firmware",
    "XR_WSL_FIRMWARE_PATH": "/mnt/wsl/xr-gate-firmware",
    "XR_WSL_FIRMWARE_FILES": "",
    "XR_BLUETOOTH_FIRMWARE_PACKAGES": "linux-firmware,zstd",
}
SERVICE = "xr-gate-wsl-firmware.service"
MAC = r"[0-9A-Fa-f]{2}(?::[0-9A-Fa-f]{2}){5}"


def packages(settings):
    result = [p.strip() for p in settings.get("XR_BLUETOOTH_FIRMWARE_PACKAGES", DEFAULTS["XR_BLUETOOTH_FIRMWARE_PACKAGES"]).split(",") if p.strip()]
    if any(not re.fullmatch(r"[a-z0-9][a-z0-9+.-]*", name) for name in result):
        raise ValueError("XR_BLUETOOTH_FIRMWARE_PACKAGES must contain literal Ubuntu package names.")
    return result


class Firmware:
    def __init__(self, settings, *, root=Path("/")):
        self.settings = {key: settings.get(key, value) for key, value in DEFAULTS.items()}
        if self.settings["XR_WSL_FIRMWARE"] not in ("0", "1"):
            raise ValueError("XR_WSL_FIRMWARE must be 0 or 1.")
        self.root = Path(root)
        self.source = self.path("XR_WSL_FIRMWARE_SOURCE")
        self.destination = self.path("XR_WSL_FIRMWARE_PATH")
        self.parameter = self.root / "sys/module/firmware_class/parameters/path"
        shared = (self.root / "mnt/wsl").resolve()
        if shared not in self.destination.resolve().parents:
            raise ValueError("XR_WSL_FIRMWARE_PATH must be a subdirectory of /mnt/wsl.")
        if self.source.resolve() in (self.root.resolve(), shared):
            raise ValueError("Refusing to expose the filesystem root as firmware.")
        if len(str(self.destination).encode()) >= 256:
            raise ValueError("XR_WSL_FIRMWARE_PATH is too long for the kernel parameter.")
        self.messages = []
        self.changed = False
        self.repaired = []

    def path(self, key):
        value = self.settings[key]
        path = PurePosixPath(value)
        if not path.is_absolute() or ".." in path.parts or any(c in value for c in "\r\n\0"):
            raise ValueError(key + " must be a literal absolute Linux path without '..'.")
        return self.root / str(path).lstrip("/")

    def command(self, args, *, check=True, timeout=30):
        result = subprocess.run([str(arg) for arg in args], capture_output=True, text=True, timeout=timeout,
                                env=dict(os.environ, DEBIAN_FRONTEND="noninteractive"))
        if check and result.returncode:
            raise RuntimeError("Command failed: " + " ".join(map(str, args)) + "\n" + (result.stderr or result.stdout).strip())
        return result

    def ensure_packages(self):
        names = packages(self.settings)
        if not names:
            return
        status = self.command(["dpkg-query", "-W", "-f=${db:Status-Status}\\n", *names], check=False)
        if status.returncode == 0 and status.stdout.splitlines() == ["installed"] * len(names):
            return
        self.messages.append("Installing Bluetooth firmware packages: " + ", ".join(names))
        self.command(["apt-get", "update"], timeout=300)
        self.command(["apt-get", "install", "-y", "--no-install-recommends", *names], timeout=900)

    def requested_files(self):
        names = [p.strip() for p in self.settings["XR_WSL_FIRMWARE_FILES"].split(",") if p.strip()]
        result = self.command(["dmesg", "--color=never"], check=False, timeout=5)
        if result.returncode == 0:
            for line in result.stdout.splitlines()[-2000:]:
                if "bluetooth" not in line.lower():
                    continue
                match = re.search(r"(?:Direct firmware load for|loading)\s+([A-Za-z0-9_+./@-]+)(?:\s|$)", line)
                if match:
                    names.append(match.group(1))
        return list(dict.fromkeys(names))

    def materialize(self, name):
        relative = PurePosixPath(name)
        if relative.is_absolute() or ".." in relative.parts or not re.fullmatch(r"[A-Za-z0-9_+./@-]+", name):
            raise ValueError("Invalid relative firmware filename: " + name)
        target = self.source / name
        if self.source.resolve() not in target.resolve().parents:
            raise ValueError("Firmware filename leaves XR_WSL_FIRMWARE_SOURCE: " + name)
        for suffix in (".zst", ".xz", ".gz"):
            compressed = target.with_name(target.name + suffix)
            if not compressed.is_file():
                continue
            if self.source.resolve() not in compressed.resolve().parents:
                raise ValueError("Compressed firmware leaves XR_WSL_FIRMWARE_SOURCE: " + name)
            if target.is_file() and target.stat().st_mtime_ns >= compressed.stat().st_mtime_ns:
                return
            temporary = None
            try:
                with tempfile.NamedTemporaryFile(dir=target.parent, prefix=".xr-gate-firmware-", delete=False) as output:
                    temporary = Path(output.name)
                    if suffix == ".zst":
                        result = subprocess.run(["zstd", "--decompress", "--stdout", str(compressed)], stdout=output, stderr=subprocess.PIPE, timeout=30)
                        if result.returncode:
                            raise RuntimeError("Could not decompress " + name + ": " + result.stderr.decode(errors="replace"))
                    else:
                        with (lzma.open if suffix == ".xz" else gzip.open)(compressed, "rb") as source:
                            shutil.copyfileobj(source, output)
                temporary.chmod(0o644)
                os.replace(temporary, target)
                self.repaired.append(name)
                self.messages.append("Prepared uncompressed firmware: " + name)
            finally:
                if temporary is not None:
                    temporary.unlink(missing_ok=True)
            return
        if not target.is_file() and name in [p.strip() for p in self.settings["XR_WSL_FIRMWARE_FILES"].split(",")]:
            raise FileNotFoundError("Configured firmware is missing: " + str(target))

    @contextmanager
    def locked(self):
        # /mnt/wsl and firmware_class.path are shared by WSL distributions.
        lock = self.root / "mnt/wsl/.xr-gate-firmware.lock"
        lock.parent.mkdir(parents=True, exist_ok=True)
        with lock.open("a") as handle:
            fcntl.flock(handle, fcntl.LOCK_EX)
            yield

    def apply(self):
        if self.settings["XR_WSL_FIRMWARE"] == "0":
            return
        with self.locked():
            self.apply_locked()

    def apply_locked(self):
        if not self.source.is_dir():
            raise FileNotFoundError("Firmware directory not found: " + str(self.source) + ". Click Install with Bluetooth enabled.")
        for name in self.requested_files():
            self.materialize(name)
        self.destination.mkdir(parents=True, exist_ok=True)
        mounted = self.command(["mountpoint", "-q", self.destination], check=False).returncode == 0
        if mounted and not os.path.samefile(self.source, self.destination):
            raise RuntimeError("The firmware mount at " + str(self.destination) + " does not belong to this Ubuntu installation. "
                               "It may remain from a deleted distribution. Save your WSL work, close XR Gate and Docker Desktop, "
                               "run wsl --shutdown in Windows PowerShell, then reopen XR Gate and retry Install.")
        if not mounted:
            self.command(["mount", "--bind", self.source, self.destination])
            self.changed = True
        if not self.parameter.is_file():
            raise RuntimeError("The WSL kernel has no firmware_class.path parameter. Check the selected kernel.")
        # sysfs adds one display newline; an additional one is part of an
        # incorrectly written path and must be repaired as well.
        if self.parameter.read_text().removesuffix("\n") != str(self.destination):
            # A newline is significant in this kernel parameter.
            self.parameter.write_text(str(self.destination))
            self.changed = True
        self.messages.append("WSL firmware available: " + str(self.destination))

    def cleanup(self):
        # Run even if firmware support is now disabled: earlier sessions may
        # still own a mount. Never detach a different distro's firmware source.
        unit = self.root / "etc/systemd/system" / SERVICE
        if unit.is_file():
            self.command(["systemctl", "disable", "--now", SERVICE], timeout=30)
        with self.locked():
            mounted = self.destination.is_dir() and self.command(["mountpoint", "-q", self.destination], check=False).returncode == 0
            if not mounted:
                self.messages.append("No firmware mount needs releasing for this Ubuntu installation.")
                return
            if not self.source.is_dir() or not os.path.samefile(self.source, self.destination):
                self.messages.append("Firmware mount belongs to a different installation; left unchanged: " + str(self.destination))
                return
            self.command(["umount", self.destination])
            if self.parameter.is_file() and self.parameter.read_text().removesuffix("\n") == str(self.destination):
                self.parameter.write_text("")
            self.changed = True
            self.messages.append("Released this Ubuntu firmware mount: " + str(self.destination))

    @staticmethod
    def write_changed(path, data):
        if path.is_file() and path.read_bytes() == data:
            return False
        path.parent.mkdir(parents=True, exist_ok=True)
        temporary = None
        try:
            with tempfile.NamedTemporaryFile(dir=path.parent, prefix=".xr-gate-", delete=False) as handle:
                temporary = Path(handle.name)
                handle.write(data)
            temporary.chmod(0o644)
            os.replace(temporary, path)
        finally:
            if temporary is not None:
                temporary.unlink(missing_ok=True)
        return True

    def install_service(self):
        helper = self.root / "usr/local/lib/xr-gate-wsl/wsl_firmware.py"
        config = self.root / "etc/xr-gate-wsl/firmware.json"
        unit = self.root / "etc/systemd/system" / SERVICE
        if self.settings["XR_WSL_FIRMWARE"] == "0":
            if unit.is_file():
                self.write_changed(config, (json.dumps(self.settings, indent=2) + "\n").encode())
                self.command(["systemctl", "disable", SERVICE])
            return
        content = ("[Unit]\nDescription=XR Gate WSL firmware access\nAfter=local-fs.target\nBefore=bluetooth.service\n\n"
                   "[Service]\nType=oneshot\nExecStart=/usr/bin/python3 /usr/local/lib/xr-gate-wsl/wsl_firmware.py boot /etc/xr-gate-wsl/firmware.json\n"
                   "RemainAfterExit=yes\nTimeoutStartSec=60\n\n[Install]\nWantedBy=multi-user.target\n")
        changed = self.write_changed(helper, Path(__file__).read_bytes())
        changed = self.write_changed(config, (json.dumps(self.settings, indent=2) + "\n").encode()) or changed
        changed = self.write_changed(unit, content.encode()) or changed
        if changed:
            self.command(["systemctl", "daemon-reload"])
        self.command(["systemctl", "enable", SERVICE])
        # apply() also runs directly on each attach, even if this oneshot is active.
        self.command(["systemctl", "start", SERVICE], timeout=65)
        self.messages.append("Automatic firmware access enabled for subsequent Ubuntu starts.")

    def selected_controller(self, usb_id):
        if not re.fullmatch(r"[0-9a-fA-F]{4}:[0-9a-fA-F]{4}", usb_id):
            raise ValueError("A selected Bluetooth VID:PID is required.")
        devices = []
        for device in (self.root / "sys/bus/usb/devices").glob("*"):
            try:
                identity = (device / "idVendor").read_text().strip() + ":" + (device / "idProduct").read_text().strip()
                if identity.lower() == usb_id.lower():
                    devices.append(device.resolve())
            except OSError:
                continue
        if len(devices) > 1:
            raise RuntimeError("Several Bluetooth dongles have USB identity " + usb_id + ". Detach the unselected dongle from WSL.")
        if not devices:
            return ""
        for hci in (self.root / "sys/class/bluetooth").glob("hci*"):
            if devices[0] not in hci.resolve().parents:
                continue
            # Newer kernels do not expose an address sysfs attribute. Match the
            # selected USB parent to hciN, then ask BlueZ for that exact adapter.
            result = self.command(["busctl", "--system", "--timeout=2s", "get-property", "org.bluez", "/org/bluez/" + hci.name, "org.bluez.Adapter1", "Address"], check=False, timeout=3)
            match = re.fullmatch(r's "(' + MAC + r')"\s*', result.stdout)
            if result.returncode == 0 and match and match.group(1) != "00:00:00:00:00:00":
                return match.group(1).upper()
        return ""

    def report(self, usb_id=""):
        try:
            address = self.selected_controller(usb_id) if usb_id else ""
        except subprocess.TimeoutExpired:
            address = ""
        return {"enabled": self.settings["XR_WSL_FIRMWARE"] == "1", "ready": bool(address), "address": address,
                "changed": self.changed, "repaired_files": self.repaired, "messages": self.messages}


def main(argv):
    if len(argv) != 3 or argv[1] not in ("install", "prepare", "status", "cleanup", "boot"):
        raise ValueError("Usage: wsl_firmware.py install|prepare|status|cleanup BASE64_JSON, or boot CONFIG_JSON")
    action = argv[1]
    if action == "boot":
        request = {"settings": json.loads(Path(argv[2]).read_text())}
    else:
        request = json.loads(base64.b64decode(argv[2], validate=True).decode("utf-8-sig"))
    settings = request.get("settings", {})
    if action == "cleanup":
        # Use the installed service's actual paths, even if Settings was edited
        # after the last installation/attachment.
        config = Path("/etc/xr-gate-wsl/firmware.json")
        if config.is_file():
            settings = json.loads(config.read_text())
    if not isinstance(settings, dict) or any(not isinstance(k, str) or not isinstance(v, str) for k, v in settings.items()):
        raise ValueError("Expected literal firmware settings.")
    manager = Firmware(settings)
    if action == "cleanup":
        if os.geteuid() != 0:
            raise RuntimeError("Firmware cleanup requires root inside Ubuntu.")
        manager.cleanup()
    elif action != "status":
        if os.geteuid() != 0:
            raise RuntimeError("Firmware setup requires root inside Ubuntu.")
        if action != "boot":
            manager.ensure_packages()
        manager.apply()
        if action != "boot":
            manager.install_service()
    if action in ("prepare", "status"):
        print(json.dumps(manager.report(request.get("bluetooth_usb_id", ""))))
    else:
        for message in manager.messages:
            print("[XR Gate WSL] " + message, flush=True)


if __name__ == "__main__":
    try:
        main(sys.argv)
    except Exception as error:
        print("[XR Gate WSL] Firmware setup failed: " + str(error), file=sys.stderr, flush=True)
        sys.exit(1)
