# XR Gate with Docker on Linux

English · [Русский](README.ru.md) · [Offline guide](START-HERE.html)

## 1. Download XR Gate

Use Ubuntu 24.04 or 26.04 on x86-64 with AVX2, ARM64 or RISC-V 64.
On RISC-V, Ubuntu 24.04 uses the RV64GC package and Ubuntu 26.04 requires
RVA23S64. Choose an `XR_VERSION` with an archive for the selected Ubuntu
image and your CPU architecture.

Download the [ZIP](https://github.com/vladoshub/xr-gate/archive/refs/heads/main.zip), extract it, and open a terminal in the `docker` folder. Run all commands below from that folder.

## 2. Install Docker

If Docker Engine 28 or newer and Docker Compose are already installed, skip this step.

```bash
sudo bash scripts/install-engine-ubuntu.sh
```

On RISC-V, the script installs Ubuntu's `docker.io`, `docker-buildx` and
`docker-compose-v2` packages.

## 3. Build XR Gate

```bash
cp --no-clobber .env.example .env
chmod +x xr-gate
```

To choose a release, change `XR_VERSION` in `.env`. Hardware settings are in `hardware/xreal_ultra.env`. For another device, copy `hardware/generic.env.example`, fill in `XR_PROFILE`, `XR_USB_IDS` and device paths, then select the new file with `XR_HARDWARE_ENV` in `.env`. Set `XR_REQUIRE_CAMERA=1` if the profile uses a camera.

Set `XR_UBUNTU_VERSION=26.04` in `.env` to use an Ubuntu 26.04 base image
and runtime. The default is `24.04`.

For a local build, set `XR_RELEASE_SOURCE=local` and place the matching
`xr-gate-ubuntu-<version>-linux-<arch>.tar.gz` and
`hand-tracking-models-mercury.tar.gz` in `docker/archives/`. The architecture
suffix is `x64`, `arm64` or `riscv64`. Set `XR_VERSION` to the corresponding
release tag. The build also installs `body-tracking-models-frame.tar.gz` if present.

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
