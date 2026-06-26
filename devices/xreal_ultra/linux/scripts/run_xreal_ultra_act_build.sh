#!/usr/bin/env bash
set -euo pipefail

log() { echo "[run_xreal_ultra_act_build] $*" >&2; }
warn() { echo "[run_xreal_ultra_act_build][WARN] $*" >&2; }
fatal() { echo "[run_xreal_ultra_act_build][ERROR] $*" >&2; exit 1; }

usage() {
  cat <<'USAGE'
Run the XREAL Ultra GitHub Actions build workflow locally through nektos/act.

By default this script:
  - installs Docker via apt when docker is missing;
  - installs the official nektos/act binary into ~/.local/bin when needed;
  - pulls the act Ubuntu 24.04 runner image once;
  - runs .github/workflows/xreal-ultra-split-build.yml through act;
  - writes act artifacts to /tmp/xr_tracking_act_artifacts;
  - writes logs to /tmp/xr_tracking_ci_logs.

Usage:
  devices/xreal_ultra/linux/scripts/run_xreal_ultra_act_build.sh [options]

Common options:
  --full                Run the whole workflow. Default.
  --list                List workflow jobs and exit.
  --models-only         Run only hand-tracking-models-mercury job.
  --job JOB             Run one workflow job, passed to act as -j JOB.
  --matrix NAME         Run one build-part matrix target, passed as
                        --matrix name:NAME. Example: --matrix steamvr
  --workflow PATH       Workflow file. Default:
                        .github/workflows/xreal-ultra-split-build.yml
  --clean-artifacts     Remove /tmp/xr_tracking_act_artifacts before running.
  --offline-actions     Pass --action-offline-mode to act. Use only after act
                        has already cached actions locally.
  --allow-pull          Allow act to pull runner images itself. By default this
                        script pre-pulls the runner image and passes --pull=false
                        to avoid parallel pull races.
  --no-image-pull       Do not pre-pull the runner image.
  --no-install-act      Do not install act automatically.
  --no-install-docker   Do not install docker.io automatically.
  -h, --help            Show this help.

Environment:
  ACT_BIN               Path to act binary. Default: auto-detect or
                        ~/.local/bin/act.
  ACT_INSTALL_DIR       Directory for auto-installed act. Default: ~/.local/bin.
  ACT_RUNNER_IMAGE      Runner image for ubuntu-24.04. Default:
                        ghcr.io/catthehacker/ubuntu:act-24.04
  ACT_ARTIFACT_DIR      Local artifact output dir. Default:
                        /tmp/xr_tracking_act_artifacts
  ACT_LOG_DIR           Local log dir. Default: /tmp/xr_tracking_ci_logs
  ACT_OFFLINE_ACTIONS   If 1, same as --offline-actions.
  ACT_INSTALL_DOCKER    If 0, same as --no-install-docker.
  ACT_INSTALL_ACT       If 0, same as --no-install-act.
USAGE
}

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
XR_ROOT_PROJECT="${XR_ROOT_PROJECT:-$(cd "$SCRIPT_DIR/../../../.." && pwd)}"
WORKFLOW="${WORKFLOW:-$XR_ROOT_PROJECT/.github/workflows/xreal-ultra-split-build.yml}"
ACT_INSTALL_DIR="${ACT_INSTALL_DIR:-$HOME/.local/bin}"
ACT_RUNNER_IMAGE="${ACT_RUNNER_IMAGE:-ghcr.io/catthehacker/ubuntu:act-24.04}"
ACT_UBUNTU_LATEST_IMAGE="${ACT_UBUNTU_LATEST_IMAGE:-$ACT_RUNNER_IMAGE}"
ACT_ARTIFACT_DIR="${ACT_ARTIFACT_DIR:-/tmp/xr_tracking_act_artifacts}"
ACT_LOG_DIR="${ACT_LOG_DIR:-/tmp/xr_tracking_ci_logs}"
INSTALL_DOCKER="${ACT_INSTALL_DOCKER:-1}"
INSTALL_ACT="${ACT_INSTALL_ACT:-1}"
OFFLINE_ACTIONS="${ACT_OFFLINE_ACTIONS:-0}"
ALLOW_PULL=0
PREPULL_IMAGE=1
CLEAN_ARTIFACTS=0
LIST_ONLY=0
JOB=""
MATRIX_NAME=""

while [[ $# -gt 0 ]]; do
  case "$1" in
    --full) JOB=""; MATRIX_NAME="" ;;
    --list) LIST_ONLY=1 ;;
    --models-only) JOB="hand-tracking-models-mercury" ;;
    --job)
      [[ $# -ge 2 ]] || fatal "--job requires a value"
      JOB="$2"
      shift
      ;;
    --matrix)
      [[ $# -ge 2 ]] || fatal "--matrix requires a value"
      JOB="build-part"
      MATRIX_NAME="$2"
      shift
      ;;
    --workflow)
      [[ $# -ge 2 ]] || fatal "--workflow requires a value"
      WORKFLOW="$2"
      shift
      ;;
    --clean-artifacts) CLEAN_ARTIFACTS=1 ;;
    --offline-actions) OFFLINE_ACTIONS=1 ;;
    --allow-pull) ALLOW_PULL=1 ;;
    --no-image-pull) PREPULL_IMAGE=0 ;;
    --no-install-act) INSTALL_ACT=0 ;;
    --no-install-docker) INSTALL_DOCKER=0 ;;
    -h|--help) usage; exit 0 ;;
    *) fatal "unknown option: $1" ;;
  esac
  shift
done

[[ "$(uname -s)" == "Linux" ]] || fatal "this script is Linux-only"
[[ -f "$WORKFLOW" ]] || fatal "workflow file not found: $WORKFLOW"
[[ -d "$XR_ROOT_PROJECT" ]] || fatal "project root not found: $XR_ROOT_PROJECT"

SUDO=()
if [[ "${EUID:-$(id -u)}" -ne 0 ]]; then
  if command -v sudo >/dev/null 2>&1; then
    SUDO=(sudo)
  fi
fi

apt_install() {
  [[ ${#SUDO[@]} -gt 0 || "${EUID:-$(id -u)}" -eq 0 ]] || fatal "sudo is required to install packages"
  DEBIAN_FRONTEND=noninteractive "${SUDO[@]}" apt-get update
  DEBIAN_FRONTEND=noninteractive "${SUDO[@]}" apt-get install -y --no-install-recommends "$@"
}

ensure_docker() {
  if ! command -v docker >/dev/null 2>&1; then
    [[ "$INSTALL_DOCKER" == "1" ]] || fatal "docker is missing; rerun without --no-install-docker or install Docker manually"
    log "docker not found; installing docker.io"
    apt_install ca-certificates curl docker.io
  fi

  if command -v systemctl >/dev/null 2>&1; then
    "${SUDO[@]}" systemctl enable --now docker >/dev/null 2>&1 || true
  fi

  local target_user="${SUDO_USER:-${USER:-}}"
  if [[ -n "$target_user" && "$target_user" != "root" ]]; then
    "${SUDO[@]}" usermod -aG docker "$target_user" >/dev/null 2>&1 || true
  fi
}

install_act() {
  mkdir -p "$ACT_INSTALL_DIR"
  log "installing nektos/act into: $ACT_INSTALL_DIR"
  command -v curl >/dev/null 2>&1 || apt_install ca-certificates curl
  curl --proto '=https' --tlsv1.2 -sSf https://raw.githubusercontent.com/nektos/act/master/install.sh \
    | bash -s -- -b "$ACT_INSTALL_DIR"
}

resolve_act() {
  if [[ -n "${ACT_BIN:-}" ]]; then
    [[ -x "$ACT_BIN" ]] || fatal "ACT_BIN is not executable: $ACT_BIN"
    return 0
  fi

  if [[ -x "$ACT_INSTALL_DIR/act" ]]; then
    ACT_BIN="$ACT_INSTALL_DIR/act"
    return 0
  fi

  if command -v act >/dev/null 2>&1; then
    ACT_BIN="$(command -v act)"
    return 0
  fi

  [[ "$INSTALL_ACT" == "1" ]] || fatal "act is missing; rerun without --no-install-act or set ACT_BIN"
  install_act
  [[ -x "$ACT_INSTALL_DIR/act" ]] || fatal "act installer did not create $ACT_INSTALL_DIR/act"
  ACT_BIN="$ACT_INSTALL_DIR/act"
}

ensure_git_repo() {
  if ! git -C "$XR_ROOT_PROJECT" rev-parse --show-toplevel >/dev/null 2>&1; then
    fatal "$XR_ROOT_PROJECT is not a git repository. Initialize/clone the repo before running act."
  fi
}

DOCKER_CMD=(docker)
ACT_CMD=()
ensure_docker
resolve_act
ensure_git_repo

if docker info >/dev/null 2>&1; then
  DOCKER_CMD=(docker)
  ACT_CMD=("$ACT_BIN")
else
  warn "current shell cannot access Docker as user $(id -un)"
  warn "if this is the first Docker install, logout/login or run: newgrp docker"
  if [[ ${#SUDO[@]} -gt 0 ]] && "${SUDO[@]}" docker info >/dev/null 2>&1; then
    warn "continuing with sudo act for this run"
    DOCKER_CMD=("${SUDO[@]}" docker)
    ACT_CMD=("${SUDO[@]}" env "HOME=$HOME" "PATH=$PATH" "$ACT_BIN")
  else
    fatal "Docker daemon is not accessible"
  fi
fi

log "project root: $XR_ROOT_PROJECT"
log "workflow: $WORKFLOW"
log "act binary: $ACT_BIN"
"${ACT_CMD[@]}" --version || true

if [[ "$PREPULL_IMAGE" == "1" ]]; then
  log "pre-pulling runner image: $ACT_RUNNER_IMAGE"
  "${DOCKER_CMD[@]}" pull "$ACT_RUNNER_IMAGE"
fi

mkdir -p "$ACT_LOG_DIR"
if [[ "$CLEAN_ARTIFACTS" == "1" ]]; then
  log "clean artifacts: $ACT_ARTIFACT_DIR"
  rm -rf "$ACT_ARTIFACT_DIR"
fi
mkdir -p "$ACT_ARTIFACT_DIR"

ACT_ARGS=(
  -W "$WORKFLOW"
  --container-architecture linux/amd64
  --artifact-server-path "$ACT_ARTIFACT_DIR"
  -P "ubuntu-24.04=$ACT_RUNNER_IMAGE"
  -P "ubuntu-latest=$ACT_UBUNTU_LATEST_IMAGE"
)

if [[ "$ALLOW_PULL" != "1" ]]; then
  ACT_ARGS+=(--pull=false)
fi
if [[ "$OFFLINE_ACTIONS" == "1" ]]; then
  ACT_ARGS+=(--action-offline-mode)
fi
if [[ -n "$JOB" ]]; then
  ACT_ARGS+=(-j "$JOB")
fi
if [[ -n "$MATRIX_NAME" ]]; then
  if [[ "$MATRIX_NAME" == *:* ]]; then
    ACT_ARGS+=(--matrix "$MATRIX_NAME")
  else
    ACT_ARGS+=(--matrix "name:$MATRIX_NAME")
  fi
fi

cd "$XR_ROOT_PROJECT"

if [[ "$LIST_ONLY" == "1" ]]; then
  exec "${ACT_CMD[@]}" -l "${ACT_ARGS[@]}"
fi

LOG_FILE="$ACT_LOG_DIR/act_xreal_ultra_$(date +%Y%m%d_%H%M%S).log"
log "log file: $LOG_FILE"
log "artifact dir: $ACT_ARTIFACT_DIR"

set +e
"${ACT_CMD[@]}" workflow_dispatch "${ACT_ARGS[@]}" 2>&1 | tee "$LOG_FILE"
status=${PIPESTATUS[0]}
set -e

if [[ ${#SUDO[@]} -gt 0 && -d "$ACT_ARTIFACT_DIR" ]]; then
  "${SUDO[@]}" chown -R "$(id -u):$(id -g)" "$ACT_ARTIFACT_DIR" "$ACT_LOG_DIR" >/dev/null 2>&1 || true
fi

if [[ "$status" -ne 0 ]]; then
  warn "act failed with status $status"
  warn "last relevant failures:"
  grep -nE '❌|Failure -|exitcode|exit code|Job failed|No space|Killed|CMake Error|ninja: build stopped|TLS handshake|EOF|failed to resolve|Could NOT find' "$LOG_FILE" | tail -n 80 >&2 || true
  exit "$status"
fi

log "act workflow completed successfully"
log "artifacts:"
find "$ACT_ARTIFACT_DIR" -type f -exec ls -lh {} \; 2>/dev/null || true

cat >&2 <<EOF2

[run_xreal_ultra_act_build] Done.

Typical artifact zip wrappers produced by act:
  $ACT_ARTIFACT_DIR/.../xreal-ultra-linux-x64.zip
  $ACT_ARTIFACT_DIR/.../hand-tracking-models-mercury.zip

Each zip contains the real release tarball, for example:
  xreal_ultra_linux_x64.tar.gz
  hand-tracking-models-mercury.tar.gz
EOF2
