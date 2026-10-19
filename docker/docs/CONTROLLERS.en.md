# Controllers with Docker

English · [Русский](CONTROLLERS.ru.md) · [Back to setup](../README.md)

## 1. Connect the controllers

Stop XR Gate. For Bluetooth controllers, run on the computer:

```bash
sudo apt-get install -y bluez
sudo systemctl enable --now bluetooth
bluetoothctl
```

For Gear VR, turn on one controller at a time and enter:

```text
power on
agent NoInputNoOutput
default-agent
scan le
```

For a VR-PARK Bluetooth joystick, use `agent on` and `scan on` instead.

Find the controller's address in the scan results. Replace `AA:BB:CC:DD:EE:FF` below with that address:

```text
pair AA:BB:CC:DD:EE:FF
trust AA:BB:CC:DD:EE:FF
```

For VR-PARK, also enter `connect AA:BB:CC:DD:EE:FF`. Repeat for the second controller if needed, then enter `scan off` and `exit`.

For a USB joystick, plug it into the computer and skip Bluetooth pairing.

## 2. Prepare the settings

Run from `docker/` after building the image:

```bash
sudo ./xr-gate export-configs
cp --no-clobber exported-configs/xr_client/xreal_ultra.json config/xr_client.json
```

If `exported-configs` already exists, reuse it or rename it before exporting again.

For **Gear VR**, run:

```bash
cp --no-clobber exported-configs/override_controller/empty_gearvr.json config/override_controller.json
```

Set `XR_BLUETOOTH=1` in `.env`.

For **VR-PARK**, run:

```bash
cp --no-clobber exported-configs/override_controller/empty_vrpark.json config/override_controller.json
cat /proc/bus/input/devices
```

Find the joystick by name and its `eventN` in the `Handlers` line. Set `XR_INPUT_DEVICES=/dev/input/eventN` in `.env`. For two devices, separate their paths with a comma.

In `config/xr_client.json`, find the existing `override_controller` service. Keep its other settings and change:

| Setting | Gear VR | VR-PARK |
| --- | --- | --- |
| `enabled` | `true` | `true` |
| `start_on_launch` | `false` | `false` |
| `env.CONFIG_PATH` | `~/.config/xr_tracking/override_controller/gearvr.json` | `~/.config/xr_tracking/override_controller/default.json` |
| `env.PROVIDERS` | `evdev,gearvr_ble` | `evdev` |
| `env.NON_INTERACTIVE` | `1` | `1` |

## 3. Register the controllers

Start XR Gate and complete the console prompts:

```bash
sudo ./xr-gate run
```

Keep the console open. Leave controller input off while registering; console command **5** toggles it.

In another terminal opened in `docker/`, run the command for your controllers.

**Gear VR:**

```bash
sudo docker exec -it \
  -e TRAIN=0 -e NON_INTERACTIVE=0 -e PROVIDERS=gearvr_ble \
  -e GEARVR_INITIAL_SCAN_MS=10000 \
  -e CONFIG_PATH=/root/.config/xr_tracking/override_controller/gearvr.json \
  xr-gate \
  /opt/xr-gate/devices/common/linux/scripts/override_controller/start_override_controller.sh --connect-devices
```

**VR-PARK:**

```bash
sudo docker exec -it xr-gate /opt/xr-gate/bin/override_controller/override_controller \
  --config /root/.config/xr_tracking/override_controller/default.json --connect-devices
```

Follow the prompts to assign the left and right devices.

## 4. Save and restart

Before stopping the container, save the result. For **Gear VR**:

```bash
sudo docker cp xr-gate:/root/.config/xr_tracking/override_controller/gearvr.json config/override_controller.json
```

For **VR-PARK**:

```bash
sudo docker cp xr-gate:/root/.config/xr_tracking/override_controller/default.json config/override_controller.json
```

After the copy succeeds, stop XR Gate:

```bash
sudo ./xr-gate stop
```

Set `start_on_launch` to `true` in the same service in `config/xr_client.json`. Start again:

```bash
sudo ./xr-gate run
```

On later starts, turn on the controllers before starting XR Gate. Repeat registration only when replacing or reassigning controllers. If the joystick's `eventN` changes, update `.env` and restart XR Gate.
