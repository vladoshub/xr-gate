#!/usr/bin/env python3
"""Render with the REAL Docker Compose CLI and verify final published ports."""
import json
from itertools import product
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
command = shlex.split(os.environ.get("XR_GATE_COMPOSE", "docker compose"))
for version, video in product(("v1.2.3", "v1.2.4"), (False, True)):
    with tempfile.TemporaryDirectory() as folder:
        override = Path(folder) / "hardware.json"
        override.write_text(json.dumps({"services": {"runtime": {
            "devices": ["/dev/video8:/dev/video0:rw", "/dev/hidraw3:/dev/hidraw3:rw"],
            "volumes": [{"type": "bind", "source": "/run/dbus/system_bus_socket",
                         "target": "/run/dbus/system_bus_socket", "read_only": True}]
        }}}))
        args = command + ["--project-directory", str(ROOT), "-f", str(ROOT / "compose.yaml"), "-f", str(override)]
        if video:
            args += ["-f", str(ROOT / "compose.video.yaml")]
        env = dict(os.environ, XR_VERSION=version, XR_PROFILE="custom_camera", XR_TARGET_DEVICE="custom_device",
                   XR_VIDEO_TCP=str(int(video)), XR_TCP_PORT="45910", XR_VIDEO_PORT="45900")
        rendered = json.loads(subprocess.check_output(args + ["config", "--format", "json"], text=True, env=env))
        service = rendered["services"]["runtime"]
        assert service["image"] == f"xr-gate-runtime:{version}", service["image"]
        assert service["build"]["args"]["XR_VERSION"] == version, service["build"]
        assert service["environment"]["XR_PROFILE"] == "custom_camera", service["environment"]
        assert service["environment"]["XR_TARGET_DEVICE"] == "custom_device", service["environment"]
        ports = service["ports"]
        assert all(p["host_ip"] == "127.0.0.1" and p["protocol"] == "tcp" for p in ports), ports
        assert {(p["target"], p["published"]) for p in ports} == ({(45810, "45910"), (45700, "45900")} if video else {(45810, "45910")})
        assert service["stdin_open"] and service["tty"] and service["init"]
        assert not service.get("privileged") and service.get("network_mode") != "host"
        assert any(v["target"] == "/config" and v["read_only"] for v in service["volumes"])
        assert any(d["target"] == "/dev/video0" for d in service["devices"])
        print(f"OK merged Compose: version={version}, video={video}, localhost ports, USB, config, stdin/tty/init")
