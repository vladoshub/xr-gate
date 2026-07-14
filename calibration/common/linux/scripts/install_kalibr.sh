#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=load_target.sh
source "$SCRIPT_DIR/load_target.sh"

log() { echo "[install_kalibr][$CALIB_TARGET] $*" >&2; }
warn() { echo "[install_kalibr][$CALIB_TARGET][WARN] $*" >&2; }
fatal() { echo "[install_kalibr][$CALIB_TARGET][ERROR] $*" >&2; exit 1; }

SUDO=()
if [[ "${EUID:-$(id -u)}" -ne 0 ]]; then
  command -v sudo >/dev/null 2>&1 || fatal "sudo is required when not running as root"
  SUDO=(sudo)
fi

DOCKER_CMD=()
setup_docker() {
  if command -v docker >/dev/null 2>&1 && docker info >/dev/null 2>&1; then
    DOCKER_CMD=(docker)
    return
  fi
  if ! command -v docker >/dev/null 2>&1; then
    [[ -r /etc/os-release ]] && source /etc/os-release
    [[ "${ID:-}" == "ubuntu" ]] || fatal "automatic Docker installation is only implemented for Ubuntu"
    "${SUDO[@]}" apt-get update
    DEBIAN_FRONTEND=noninteractive "${SUDO[@]}" apt-get install -y --no-install-recommends docker.io
  fi
  if command -v systemctl >/dev/null 2>&1; then
    "${SUDO[@]}" systemctl enable --now docker || true
  fi
  local target_user="${SUDO_USER:-${USER:-}}"
  if [[ -n "$target_user" && "$target_user" != root ]]; then
    "${SUDO[@]}" usermod -aG docker "$target_user" || true
  fi
  if docker info >/dev/null 2>&1; then
    DOCKER_CMD=(docker)
  elif [[ ${#SUDO[@]} -gt 0 ]] && "${SUDO[@]}" docker info >/dev/null 2>&1; then
    warn "using sudo docker for this run; log out/in later for docker-group access"
    DOCKER_CMD=("${SUDO[@]}" docker)
  else
    fatal "Docker daemon is not reachable"
  fi
}

setup_docker
print_target_summary

mkdir -p "$CALIB_WORK_DIR/docker" "$TARGETS_DIR" "$CALIB_WORK_DIR/tools" "$BAGS_DIR" "$KALIBR_RUNS_DIR" "$CAMERA_PROFILE_DIR" "$FINAL_PROFILE_DIR"

DOCKERFILE="$CALIB_WORK_DIR/docker/Dockerfile.kalibr-xr-gate"
cat > "$DOCKERFILE" <<'DOCKER'
FROM christianbrommer/kalibr:latest
ENV DEBIAN_FRONTEND=noninteractive
RUN set -eux; \
    mkdir -p /tmp/disabled-apt-sources; \
    find /etc/apt/sources.list.d -maxdepth 1 -type f \
      \( -iname '*ros*' -o -iname '*gazebo*' \) \
      -exec mv {} /tmp/disabled-apt-sources/ \; || true; \
    apt-get update; \
    apt-get install -y --no-install-recommends \
      python3-opencv python3-yaml python3-numpy python3-pip python3-pil; \
    rm -rf /var/lib/apt/lists/*
DOCKER

"${DOCKER_CMD[@]}" build \
  ${KALIBR_DOCKER_NO_CACHE:+--no-cache} \
  -t "$DOCKER_IMAGE" \
  -f "$DOCKERFILE" \
  "$CALIB_WORK_DIR/docker"

"${DOCKER_CMD[@]}" run --rm \
  -v "$CALIB_WORK_DIR:$CALIB_WORK_DIR" \
  -w "$CALIB_WORK_DIR" \
  "$DOCKER_IMAGE" \
  bash -lc '
    set -e
    source /kalibr/catkin_ws/devel/setup.bash
    command -v kalibr_calibrate_cameras
    command -v kalibr_calibrate_imu_camera
    python3 - <<PY
import cv2, yaml, numpy
print("cv2", cv2.__version__)
print("numpy", numpy.__version__)
print("kalibr container deps OK")
PY
  '

if [[ ! -f "$APRILGRID_PATH" || "${FORCE_TARGET_CONFIGS:-0}" == "1" ]]; then
  cat > "$APRILGRID_PATH" <<EOF_GRID
target_type: 'aprilgrid'
tagCols: ${APRILGRID_TAG_COLS:-5}
tagRows: ${APRILGRID_TAG_ROWS:-7}
tagSize: ${APRILGRID_TAG_SIZE_M:-0.02593}
tagSpacing: ${APRILGRID_TAG_SPACING:-0.4}
EOF_GRID
fi

if [[ ! -f "$IMU_YAML_PATH" || "${FORCE_TARGET_CONFIGS:-0}" == "1" ]]; then
  cat > "$IMU_YAML_PATH" <<EOF_IMU
# ${IMU_PROFILE_NOTE:-Initial calibration values; verify for the final hardware.}
rostopic: $IMU_TOPIC
update_rate: ${IMU_UPDATE_RATE:-200.0}

accelerometer_noise_density: ${IMU_ACCEL_NOISE_DENSITY:-0.01}
accelerometer_random_walk: ${IMU_ACCEL_RANDOM_WALK:-0.001}
gyroscope_noise_density: ${IMU_GYRO_NOISE_DENSITY:-0.001}
gyroscope_random_walk: ${IMU_GYRO_RANDOM_WALK:-0.0001}
EOF_IMU
fi

echo
echo "[OK] Docker image: $DOCKER_IMAGE"
echo "[OK] AprilGrid: $APRILGRID_PATH"
cat "$APRILGRID_PATH"
echo
echo "[OK] IMU profile: $IMU_YAML_PATH"
cat "$IMU_YAML_PATH"
