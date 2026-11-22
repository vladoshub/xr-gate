# XR Gate with Docker on Linux

English · [Русский](README.ru.md) · [Offline guide](START-HERE.html)

## 1. Download XR Gate

Use Ubuntu 24.04 on x86-64 with AVX2 or on ARM64. Select an `XR_VERSION`
that publishes the Ubuntu 24.04 runtime for your architecture.

Download the [ZIP](https://github.com/vladoshub/xr-gate/archive/refs/heads/main.zip), extract it, and open a terminal in the `docker` folder. Run all commands below from that folder.

## 2. Install Docker

If Docker Engine 28 or newer and Docker Compose are already installed, skip this step.

```bash
sudo bash scripts/install-engine-ubuntu.sh
```

## 3. Build XR Gate

```bash
cp --no-clobber .env.example .env
chmod +x xr-gate
```

To choose a release, change `XR_VERSION` in `.env`. Hardware settings are in `hardware/xreal_ultra.env`. For another device, copy `hardware/generic.env.example`, fill in `XR_PROFILE`, `XR_USB_IDS` and device paths, then select the new file with `XR_HARDWARE_ENV` in `.env`. Set `XR_REQUIRE_CAMERA=1` if the profile uses a camera.

For a local build, set `XR_RELEASE_SOURCE=local` in `.env` and place the
Ubuntu 24.04 runtime archive for your architecture
(`xr-gate-ubuntu-24.04-linux-x64.tar.gz` or `xr-gate-ubuntu-24.04-linux-arm64.tar.gz`)
and `hand-tracking-models-mercury.tar.gz` in `docker/archives/`.
Set `XR_VERSION` to the corresponding release tag. The build also installs
`body-tracking-models-frame.tar.gz` if present in that directory.

Then build the image:

```bash
sudo ./xr-gate build
```

Docker may still need internet access to download the base image and Ubuntu packages.

## 4. Start XR Gate

Connect the selected device, then run:

```bash
sudo ./xr-gate run
```

Follow the prompts in the console and keep it open. If the glasses are already in SBS mode, choose the current display mode when prompted.

## 5. Stop and start again

Stop XR Gate with **Ctrl+C**, or run this in another terminal:

```bash
sudo ./xr-gate stop
```

For the next session, run `sudo ./xr-gate run` again.

After reconnecting the device, stop XR Gate and start it again.

## 6. Optional setup

- [Gear VR and VR-PARK controllers](docs/CONTROLLERS.en.md).
- [Custom configuration](config/README.md).
- To change the release: stop XR Gate, edit `XR_VERSION` in `.env`, then repeat the build and start steps.

## 7. If startup fails

Check the connected devices:

```bash
sudo ./xr-gate doctor
```

To see the running container's logs:

```bash
sudo ./xr-gate logs
```
