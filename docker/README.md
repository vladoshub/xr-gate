# XR Gate with Docker on Linux

English · [Русский](README.ru.md) · [Offline guide](START-HERE.html)

## 1. Download XR Gate

Use Ubuntu 24.04 (64-bit) and a CPU with AVX2 support.

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

To choose a release, change `XR_VERSION` in `.env`. Then build the image:

```bash
sudo ./xr-gate build
```

Wait for the build to finish. The first build needs an internet connection.

## 4. Start XR Gate

Connect the XREAL glasses, then run:

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

After reconnecting the glasses, stop XR Gate and start it again.

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
