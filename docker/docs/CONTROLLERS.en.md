# Physical controllers with Docker

[Русский](CONTROLLERS.ru.md) · English

The default release profile does not auto-start physical controller input.
Use the root README's supplied templates and **--connect-devices** for Gear VR
or VR-PARK; full training is for other layouts. Config overrides remain on the
host; BlueZ runs on the Linux/WSL host.

## Gear VR: host pairing

Stop XR Gate. Forward a dedicated Bluetooth dongle into Ubuntu when using WSL.
On the Linux host:

```bash
sudo systemctl enable --now bluetooth
bluetoothctl
```

Turn on only the first Gear VR initially. Inside bluetoothctl:

```text
power on
agent NoInputNoOutput
default-agent
scan le
```

Find its actual MAC. Run `pair <MAC>`, `trust <MAC>`, `info <MAC>`. Turn on the
second controller and repeat for its MAC, then `scan off`, `exit`. If several
adapters exist, select the correct one with `list` and `select <adapter MAC>`.
Set **XR_BLUETOOTH=1** in the Linux copy of `.env`.

## Prepare the supplied config

From the repository's `docker/` folder, after building the image:

```bash
sudo ./xr-gate export-configs
cp --no-clobber exported-configs/xr_client/xreal_ultra.json config/xr_client.json
cp --no-clobber exported-configs/override_controller/empty_gearvr.json config/override_controller.json
```

An existing export/config is deliberately not overwritten; use it or back it
up/rename it before starting from another template. In the **existing**
`override_controller` service in `config/xr_client.json`, keep all unrelated
fields and set:

- `enabled`: true.
- `start_on_launch`: **false during registration**, then true after completion.
- `env.CONFIG_PATH`: `~/.config/xr_tracking/override_controller/gearvr.json`.
- `env.PROVIDERS`: `evdev,gearvr_ble`.
- `env.NON_INTERACTIVE`: `1` for ordinary runtime use.

## Register devices without retraining buttons

Start `sudo ./xr-gate run` and complete its startup gate. Do not activate the
override service with console command 5 while registering. If it was already
running, stop that service first. In another Linux terminal:

```bash
sudo docker exec -it \
  -e TRAIN=0 -e NON_INTERACTIVE=0 -e PROVIDERS=gearvr_ble \
  -e GEARVR_INITIAL_SCAN_MS=10000 -e VERBOSE=1 \
  -e CONFIG_PATH=/root/.config/xr_tracking/override_controller/gearvr.json \
  xr-gate \
  /opt/xr-gate/devices/common/linux/scripts/override_controller/start_override_controller.sh --connect-devices
```

Follow the device/left/right prompts. This uses the template's bindings and
parameters; it does not start a full button-training wizard.

Export the registered result **before the next container start**, because the
host override is copied into the state volume at startup:

```bash
sudo docker cp xr-gate:/root/.config/xr_tracking/override_controller/gearvr.json config/override_controller.connected.json
python3 -m json.tool config/override_controller.connected.json > /dev/null
cp --no-clobber config/override_controller.json config/override_controller.before-connect.json
sudo cp config/override_controller.connected.json config/override_controller.json
```

Check each command succeeds. Now stop XR Gate, set `start_on_launch: true` in the
client override, and recreate the container. Physical input will auto-start;
console command **5** toggles it manually. Pairing and registration normally
need doing only once. XR Gate's ordinary startup IMU alignment still applies.

## VR-PARK / ordinary evdev

Pair the joystick on the Linux host (`agent on`, `scan on`, then pair/trust and
connect its actual MAC). USB joysticks must be attached to Linux/WSL. Identify
the required `/dev/input/eventN` nodes and list only those in **XR_INPUT_DEVICES**
in `.env`; recreate the container when device nodes change.

Use **empty_vrpark.json** instead of the Gear VR template. Set the service's
`CONFIG_PATH` to `~/.config/xr_tracking/override_controller/default.json` and
`PROVIDERS` to `evdev`. Keep auto-start off until registration. In the running
container, with no other override process active:

```bash
sudo docker exec -it xr-gate /opt/xr-gate/bin/override_controller/override_controller \
  --config /root/.config/xr_tracking/override_controller/default.json --connect-devices
```

Export the updated **default.json** back to the host override as above, then
enable auto-start and restart. The template may suit similar four-button,
mouse-stick joysticks; compatibility is device-specific.

## Other devices: full training

For a genuinely different evdev layout, the root README's training launcher is
available inside the running container. Stop the override service first, back
up existing config, and run the same common script with `TRAIN=1`,
`NON_INTERACTIVE=0`, `PROVIDERS=evdev` and explicit `CONFIG_PATH` to **default.json**.
After successful training, export the result to the host override before
restarting. This replaces bindings, unlike **--connect-devices**.

The `controller-state` volume persists controller JSON. Host BlueZ persists
Bluetooth pairing independently. Do not delete the volume to troubleshoot a
temporary connection unless you have saved any needed configuration.
