#!/usr/bin/env bash
# Run on the Ubuntu host, never inside the application container.
set -euo pipefail
if [[ "$(id -u)" != 0 ]]; then
  exec sudo bash "$0" "$@"
fi
source /etc/os-release
if [[ "${ID:-}" != ubuntu || "${VERSION_ID:-}" != 24.04 ]]; then
  echo 'This host installer targets Ubuntu 24.04. Other Docker-capable Linux hosts can use the bundle manually.' >&2
  exit 1
fi
if [[ "$(uname -m)" != x86_64 ]]; then
  echo 'This release requires an x86-64 CPU with AVX2 support.' >&2
  exit 1
fi

if [[ "$(ps -p 1 -o comm=)" != systemd ]]; then
  echo 'systemd is required by this host installer for Docker and BlueZ.' >&2
  exit 1
fi

# Do not remove an existing distro-supplied engine or container runtime silently.
for conflict in docker.io docker-compose docker-compose-v2 podman-docker containerd runc; do
  if dpkg-query -W -f='${Status}' "$conflict" 2>/dev/null | grep -q 'install ok installed'; then
    echo "Existing package $conflict conflicts with Docker CE. Follow the official migration instructions, then rerun:" >&2
    echo 'https://docs.docker.com/engine/install/ubuntu/' >&2
    exit 1
  fi
done

export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y ca-certificates curl python3 usbutils udev v4l-utils dbus bluez rfkill linux-firmware
install -m 0755 -d /etc/apt/keyrings
curl -fsSL https://download.docker.com/linux/ubuntu/gpg -o /etc/apt/keyrings/docker.asc
chmod a+r /etc/apt/keyrings/docker.asc
cat > /etc/apt/sources.list.d/docker.sources <<EOF
Types: deb
URIs: https://download.docker.com/linux/ubuntu
Suites: ${UBUNTU_CODENAME:-$VERSION_CODENAME}
Components: stable
Architectures: $(dpkg --print-architecture)
Signed-By: /etc/apt/keyrings/docker.asc
EOF
apt-get update
apt-get install -y docker-ce docker-ce-cli containerd.io docker-buildx-plugin docker-compose-plugin
systemctl enable --now docker bluetooth

for module in uvcvideo hid_generic btusb; do
  if ! modprobe "$module"; then
    echo "Module $module unavailable. Install the matching Linux kernel modules, then run sudo ./xr-gate doctor." >&2
  fi
done
docker version
docker compose version
echo 'Host ready. Use sudo ./xr-gate build, then sudo ./xr-gate run.'
