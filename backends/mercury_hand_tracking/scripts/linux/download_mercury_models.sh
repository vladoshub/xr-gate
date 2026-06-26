#!/usr/bin/env bash
set -euo pipefail

# Download/copy Mercury hand-tracking ONNX models into a runtime models folder.
# Default destination in source builds:
#   $ROOT_PROJECT/bin/hand-tracking-models/mercury
# Device/package wrappers override this to:
#   out/xreal_ultra/bin/hand-tracking-models/mercury

expand_tilde() {
  local value="$1"
  case "$value" in
    "~") printf '%s\n' "$HOME" ;;
    "~/"*) printf '%s\n' "$HOME/${value#"~/"}" ;;
    *) printf '%s\n' "$value" ;;
  esac
}

log() {
  printf '\n\033[1;32m[download_mercury_models]\033[0m %s\n' "$*" >&2
}

fail() {
  printf '\n\033[1;31m[download_mercury_models][ERROR]\033[0m %s\n' "$*" >&2
  exit 1
}

require_file() {
  local p="$1"
  [[ -f "$p" ]] || fail "required file not found: $p"
}

ROOT_PROJECT="${ROOT_PROJECT:-$HOME/src/xr_tracking}"
ROOT_PROJECT="$(expand_tilde "$ROOT_PROJECT")"
THIRD_PARTY_DIR="${THIRD_PARTY_DIR:-$ROOT_PROJECT/third_party}"
THIRD_PARTY_DIR="$(expand_tilde "$THIRD_PARTY_DIR")"
MERCURY_DRIVER_DIR="${MERCURY_DRIVER_DIR:-$THIRD_PARTY_DIR/mercury_steamvr_driver}"
MERCURY_DRIVER_DIR="$(expand_tilde "$MERCURY_DRIVER_DIR")"
MERCURY_DRIVER_REPO_URL="${MERCURY_DRIVER_REPO_URL:-https://github.com/moshimeow/mercury_steamvr_driver.git}"
MERCURY_DRIVER_REF="${MERCURY_DRIVER_REF:-e3948ace94a9f2cbd949adf50ffcc082002337cc}"
CLONE_MERCURY_DRIVER="${CLONE_MERCURY_DRIVER:-1}"
FETCH_MERCURY_DRIVER="${FETCH_MERCURY_DRIVER:-1}"
RESET_MERCURY_DRIVER_TREE="${RESET_MERCURY_DRIVER_TREE:-1}"
INSTALL_LFS_DEPS="${INSTALL_LFS_DEPS:-1}"
MERCURY_MODELS="${MERCURY_MODELS:-$ROOT_PROJECT/bin/hand-tracking-models/mercury}"
MERCURY_MODELS="$(expand_tilde "$MERCURY_MODELS")"

MODEL_SRC_DIR="$MERCURY_DRIVER_DIR/src/steamvr_driver/mercury/resources/internal/hand-tracking-models"
DETECTION_MODEL="grayscale_detection_160x160.onnx"
KEYPOINT_MODEL="grayscale_keypoint_jan18.onnx"

if [[ "$INSTALL_LFS_DEPS" == "1" ]]; then
  if ! command -v git-lfs >/dev/null 2>&1; then
    log "Installing git-lfs dependency"
    sudo apt update
    sudo apt install -y git-lfs
  fi
fi

if [[ -d "$MERCURY_DRIVER_DIR/.git" ]]; then
  log "Using existing Mercury SteamVR driver source tree: $MERCURY_DRIVER_DIR"
else
  if [[ "$CLONE_MERCURY_DRIVER" != "1" ]]; then
    fail "Mercury SteamVR driver source tree not found: $MERCURY_DRIVER_DIR
Clone it first, or run with CLONE_MERCURY_DRIVER=1."
  fi
  log "Cloning Mercury SteamVR driver into: $MERCURY_DRIVER_DIR"
  mkdir -p "$(dirname "$MERCURY_DRIVER_DIR")"
  git clone "$MERCURY_DRIVER_REPO_URL" "$MERCURY_DRIVER_DIR"
fi

if [[ "$FETCH_MERCURY_DRIVER" == "1" ]]; then
  log "Fetching Mercury SteamVR driver refs/tags"
  git -C "$MERCURY_DRIVER_DIR" fetch --tags origin
fi

if ! git -C "$MERCURY_DRIVER_DIR" rev-parse --verify --quiet "${MERCURY_DRIVER_REF}^{commit}" >/dev/null; then
  fail "Mercury SteamVR driver ref/commit not found: $MERCURY_DRIVER_REF in $MERCURY_DRIVER_DIR"
fi

cd "$MERCURY_DRIVER_DIR"
if [[ "$RESET_MERCURY_DRIVER_TREE" == "1" ]]; then
  log "Resetting Mercury SteamVR driver to: $MERCURY_DRIVER_REF"
  git checkout "$MERCURY_DRIVER_REF"
  git reset --hard "$MERCURY_DRIVER_REF"
  git clean -fd
else
  git checkout "$MERCURY_DRIVER_REF"
fi

if command -v git-lfs >/dev/null 2>&1; then
  git lfs install
  git submodule update --init --recursive
  git lfs pull || true
  git submodule foreach --recursive 'git lfs install && git lfs pull || true'
else
  log "git-lfs not found; continuing, but model files may be LFS pointer files"
fi

require_file "$MODEL_SRC_DIR/$DETECTION_MODEL"
require_file "$MODEL_SRC_DIR/$KEYPOINT_MODEL"

mkdir -p "$MERCURY_MODELS"
cp "$MODEL_SRC_DIR/$DETECTION_MODEL" "$MERCURY_MODELS/"
cp "$MODEL_SRC_DIR/$KEYPOINT_MODEL" "$MERCURY_MODELS/"

log "Installed Mercury models:"
ls -lh "$MERCURY_MODELS/$DETECTION_MODEL" "$MERCURY_MODELS/$KEYPOINT_MODEL"
