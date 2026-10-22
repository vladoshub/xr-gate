"""A stateful bluetoothctl stand-in; tests never contact the system bus."""
import json
import os
from pathlib import Path
import sys


config = json.loads(os.environ["XR_TEST_BLUETOOTH_SESSION"])
controllers = config["adapters"]
selected = next(iter(controllers))  # Intentionally defaults to the built-in.
devices = config["devices"]
mode = config.get("mode", "")
log = Path(config["log"])
available = True
print("Agent registered", flush=True)


def emit(text):
    # Prompts/CR, ANSI and byte-sized chunks exercise the stream parser.
    payload = ("[bluetooth]# \r\x1b[0m" + text + "\n[bluetooth]# ").encode()
    if config.get("fragmented"):
        for byte in payload:
            os.write(sys.stdout.fileno(), bytes([byte]))
    else:
        os.write(sys.stdout.fileno(), payload)


for line in sys.stdin:
    command = line.strip().split()
    if not command:
        continue
    with log.open("a") as stream:
        stream.write(json.dumps({"command": command, "adapter": selected}) + "\n")
    action = command[0]
    if action == "quit":
        break
    if action == "select":
        mac = command[1]
        if mode == "select-eof":
            break
        if mac not in controllers or mode == "reject-select" or not available:
            emit("Controller " + mac + " not available")
        else:
            selected = mac
            emit("Controller " + selected + " Adapter [default]")
        continue
    if action == "show":
        powered = "yes" if controllers[selected]["powered"] else "no"
        emit("Controller " + selected + " (public)\n\tName: Adapter\n\tPowered: " + powered)
        continue
    if action == "agent":
        emit("Agent is already registered")
        continue
    if action == "scan":
        state = "started" if command[1] == "on" else "stopped"
        emit("Discovery " + state)
        # Foreign-adapter events must not become selectable device-list entries.
        emit("[NEW] Device FF:FF:FF:FF:FF:FF Foreign advertisement")
        continue
    if action == "devices":
        emit("\n".join("Device " + mac + " " + device["name"]
                       for mac, device in devices[selected].items()))
        continue
    if action == "power":
        raise AssertionError("Pairing must never change any adapter's power")
    mac = command[1]
    device = devices[selected].get(mac)
    if not device:
        emit("Device " + mac + " not available")
        continue
    if action == "info":
        properties = "\n".join("\t" + key.capitalize() + ": " + ("yes" if device.get(key) else "no")
                               for key in ("paired", "trusted", "connected"))
        emit("Device " + mac + " (public)\n\tName: " + device["name"] +
             "\n\tAlias: " + device["name"] + "\n" + properties)
        if mode == "drop-after-info":
            available = False
    elif action == "pair":
        if mode == "pair-timeout":
            emit("Attempting to pair with " + mac)
        elif mode == "pair-error":
            emit("Failed to pair: org.bluez.Error.AuthenticationFailed")
        else:
            if mode != "pair-lies":
                device["paired"] = True
            emit("Pairing successful")
    elif action == "trust":
        device["trusted"] = True
        emit("Changing " + mac + " trust succeeded")
    elif action == "connect":
        device["connected"] = True
        emit("Connection successful")
    else:
        raise AssertionError("Unexpected command: " + " ".join(command))
