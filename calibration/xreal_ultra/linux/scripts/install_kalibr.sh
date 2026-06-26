#!/usr/bin/env bash
set -euo pipefail

log() { echo "[install_kalibr] $*" >&2; }
warn() { echo "[install_kalibr][WARN] $*" >&2; }
fatal() { echo "[install_kalibr][ERROR] $*" >&2; exit 1; }

SUDO=()
if [[ "${EUID:-$(id -u)}" -ne 0 ]]; then
  if command -v sudo >/dev/null 2>&1; then
    SUDO=(sudo)
  else
    fatal "sudo is required to install/start Docker when not running as root"
  fi
fi

DOCKER_CMD=()

setup_docker() {
  if command -v docker >/dev/null 2>&1 && docker info >/dev/null 2>&1; then
    DOCKER_CMD=(docker)
    return 0
  fi

  if ! command -v docker >/dev/null 2>&1; then
    log "Docker is not installed; installing docker.io through apt"
    if [[ -r /etc/os-release ]]; then
      # shellcheck source=/dev/null
      source /etc/os-release
      if [[ "${ID:-}" != "ubuntu" ]]; then
        fatal "automatic Docker installation is only implemented for Ubuntu; install Docker manually and rerun"
      fi
    fi
    "${SUDO[@]}" apt-get update
    DEBIAN_FRONTEND=noninteractive "${SUDO[@]}" apt-get install -y --no-install-recommends docker.io
  fi

  if command -v systemctl >/dev/null 2>&1; then
    log "starting Docker service"
    "${SUDO[@]}" systemctl enable --now docker || true
  else
    warn "systemctl not found; assuming Docker daemon is already managed by this environment"
  fi

  local target_user="${SUDO_USER:-${USER:-}}"
  if [[ -n "$target_user" && "$target_user" != "root" ]]; then
    log "adding user '$target_user' to docker group"
    "${SUDO[@]}" usermod -aG docker "$target_user" || true
  fi

  if docker info >/dev/null 2>&1; then
    DOCKER_CMD=(docker)
    return 0
  fi

  if [[ ${#SUDO[@]} -gt 0 ]] && "${SUDO[@]}" docker info >/dev/null 2>&1; then
    warn "current shell does not have docker group permissions yet; using sudo docker for this run"
    warn "logout/login or reboot later to use docker without sudo"
    DOCKER_CMD=("${SUDO[@]}" docker)
    return 0
  fi

  fatal "Docker is installed but the daemon is not reachable; check 'systemctl status docker'"
}

setup_docker

XREAL_CALIB_DIR="${XREAL_CALIB_DIR:-$HOME/xreal_calib}"
XREAL_CALIB_DIR="${XREAL_CALIB_DIR/#\~/$HOME}"

# create local kalibr image
mkdir -p "$XREAL_CALIB_DIR/docker"
mkdir -p "$XREAL_CALIB_DIR/targets"
mkdir -p "$XREAL_CALIB_DIR/tools"
mkdir -p "$XREAL_CALIB_DIR/bags"
mkdir -p "$XREAL_CALIB_DIR/kalibr_runs"
mkdir -p "$XREAL_CALIB_DIR/unified_480"

cat > "$XREAL_CALIB_DIR/docker/Dockerfile.kalibr-xreal" <<'DOCKER'
FROM christianbrommer/kalibr:latest

ENV DEBIAN_FRONTEND=noninteractive

# christianbrommer/kalibr is based on old Ubuntu/ROS apt sources.
# On fresh machines apt-get update may fail with:
# EXPKEYSIG F42ED6FBAB17C654 Open Robotics
#
# Kalibr is already installed in the base image, so for our extra Python deps
# we temporarily disable ROS/Gazebo apt sources and use Ubuntu bionic repos only.
RUN set -eux; \
    mkdir -p /tmp/disabled-apt-sources; \
    find /etc/apt/sources.list.d -maxdepth 1 -type f \
      \( -iname '*ros*' -o -iname '*gazebo*' \) \
      -exec mv {} /tmp/disabled-apt-sources/ \; || true; \
    apt-get update; \
    apt-get install -y --no-install-recommends \
      python3-opencv \
      python3-yaml \
      python3-numpy \
      python3-pip \
      python3-pil; \
    rm -rf /var/lib/apt/lists/*
DOCKER

"${DOCKER_CMD[@]}" build \
  --no-cache \
  -t kalibr-xreal:latest \
  -f "$XREAL_CALIB_DIR/docker/Dockerfile.kalibr-xreal" \
  "$XREAL_CALIB_DIR/docker"

echo
echo "[OK] kalibr-xreal:latest built"

"${DOCKER_CMD[@]}" run --rm \
  -v "$XREAL_CALIB_DIR:$XREAL_CALIB_DIR" \
  -w "$XREAL_CALIB_DIR" \
  kalibr-xreal:latest \
  bash -lc '
    set -e
    source /kalibr/catkin_ws/devel/setup.bash
    command -v kalibr_calibrate_cameras
    command -v kalibr_calibrate_imu_camera
    command -v kalibr_create_target_pdf || true
    python3 - <<PY
import cv2, yaml, numpy
print("cv2", cv2.__version__)
print("numpy", numpy.__version__)
print("kalibr container deps OK")
PY
  '

cat > "$XREAL_CALIB_DIR/targets/aprilgrid_a4_5x7_25mm.yaml" <<'TARGET'
target_type: 'aprilgrid'
tagCols: 5
tagRows: 7
tagSize: 0.02593
tagSpacing: 0.4
TARGET

cat > "$XREAL_CALIB_DIR/imu_xreal_air2ultra.yaml" <<'IMU'
rostopic: /imu0
update_rate: 1000.0

accelerometer_noise_density: 0.01
accelerometer_random_walk: 0.001
gyroscope_noise_density: 0.001
gyroscope_random_walk: 0.0001
IMU

echo
echo "[OK] target YAML:"
cat "$XREAL_CALIB_DIR/targets/aprilgrid_a4_5x7_25mm.yaml"

echo
echo "[OK] IMU YAML:"
cat "$XREAL_CALIB_DIR/imu_xreal_air2ultra.yaml"
