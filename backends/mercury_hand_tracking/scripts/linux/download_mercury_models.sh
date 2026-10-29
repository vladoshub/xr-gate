#!/usr/bin/env bash
set -euo pipefail

# Download/copy Mercury hand-tracking ONNX models into a runtime models folder.
# Default destination in source builds:
#   $ROOT_PROJECT/bin/hand-tracking-models/mercury
# Device/package wrappers override this to:
#   out/xr-gate/bin/hand-tracking-models/mercury

_dependency_support="$(dirname "${BASH_SOURCE[0]}")"
if [[ -f "$_dependency_support/dependency_policy.sh" ]]; then
  source "$_dependency_support/dependency_policy.sh"  # Packaged runtime helper
else
  source "$_dependency_support/../../../../devices/common/linux/scripts/build/dependency_policy.sh"
fi
unset _dependency_support

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
FETCH_MERCURY_DRIVER="${FETCH_MERCURY_DRIVER:-$XR_UPDATE_DEPS}"
RESET_MERCURY_DRIVER_TREE="${RESET_MERCURY_DRIVER_TREE:-1}"
MERCURY_MODELS="${MERCURY_MODELS:-$ROOT_PROJECT/bin/hand-tracking-models/mercury}"
MERCURY_MODELS="$(expand_tilde "$MERCURY_MODELS")"

MODEL_SRC_DIR="$MERCURY_DRIVER_DIR/src/steamvr_driver/mercury/resources/internal/hand-tracking-models"
DETECTION_MODEL="grayscale_detection_160x160.onnx"
KEYPOINT_MODEL="grayscale_keypoint_jan18.onnx"

command -v git-lfs >/dev/null 2>&1 || fail "git-lfs not found. Run XR_BUILD_ONLY=hand_tracking_models ./devices/common/linux/scripts/build/install_build_deps_ubuntu.sh before downloading models."

MERCURY_CHECKOUT_REF="$(xr_ensure_git_source "$MERCURY_DRIVER_DIR" "$MERCURY_DRIVER_REPO_URL" "$MERCURY_DRIVER_REF" "$CLONE_MERCURY_DRIVER" "$FETCH_MERCURY_DRIVER")" || exit $?

cd "$MERCURY_DRIVER_DIR"
if [[ "$RESET_MERCURY_DRIVER_TREE" == "1" ]]; then
  log "Resetting Mercury SteamVR driver to: $MERCURY_DRIVER_REF"
  git checkout "$MERCURY_CHECKOUT_REF"
  git reset --hard "$MERCURY_CHECKOUT_REF"
  git clean -fd
else
  git checkout "$MERCURY_CHECKOUT_REF"
fi

xr_update_submodules "$MERCURY_DRIVER_DIR"
git lfs install --local
git lfs checkout
git submodule foreach --recursive 'git lfs install --local && git lfs checkout'

models_ready() {
  local model
  for model in "$DETECTION_MODEL" "$KEYPOINT_MODEL"; do
    [[ -s "$MODEL_SRC_DIR/$model" ]] || return 1
    if head -c 128 "$MODEL_SRC_DIR/$model" | grep -F 'version https://git-lfs.github.com/spec/v1' >/dev/null; then
      return 1
    fi
  done
}
if [[ "$XR_UPDATE_DEPS" == 1 ]] || ! models_ready; then
  xr_require_network "Mercury model LFS objects missing or refresh requested"
  git lfs pull
  git submodule foreach --recursive 'git lfs pull'
fi
models_ready || fail "Mercury models missing or still LFS pointers after hydration: $MODEL_SRC_DIR"

mkdir -p "$MERCURY_MODELS"
cp "$MODEL_SRC_DIR/$DETECTION_MODEL" "$MERCURY_MODELS/"
cp "$MODEL_SRC_DIR/$KEYPOINT_MODEL" "$MERCURY_MODELS/"

MERCURY_MODEL_LICENSE="$MERCURY_DRIVER_DIR/LICENSES/BSL-1.0.txt"
require_file "$MERCURY_MODEL_LICENSE"
rm -rf "$MERCURY_MODELS/licenses"
mkdir -p "$MERCURY_MODELS/licenses"
cp "$MERCURY_MODEL_LICENSE" "$MERCURY_MODELS/licenses/BSL-1.0.txt"
MERCURY_DRIVER_RESOLVED_COMMIT="$(git -C "$MERCURY_DRIVER_DIR" rev-parse HEAD)"
cat > "$MERCURY_MODELS/SOURCE_INFO.txt" <<EOF_MODEL_SOURCE
Component: Mercury hand-tracking ONNX models
Upstream repository: $MERCURY_DRIVER_REPO_URL
Requested ref: $MERCURY_DRIVER_REF
Resolved commit: $MERCURY_DRIVER_RESOLVED_COMMIT
Repository license file packaged by upstream: BSL-1.0
XR Gate does not relicense or independently certify the provenance of the model weights.
Models:
  $DETECTION_MODEL
  $KEYPOINT_MODEL
EOF_MODEL_SOURCE
(
  cd "$MERCURY_MODELS"
  sha256sum "$DETECTION_MODEL" "$KEYPOINT_MODEL" "licenses/BSL-1.0.txt" > SHA256SUMS.txt
)

log "Installed Mercury models and legal metadata:"
ls -lh \
  "$MERCURY_MODELS/$DETECTION_MODEL" \
  "$MERCURY_MODELS/$KEYPOINT_MODEL" \
  "$MERCURY_MODELS/licenses/BSL-1.0.txt" \
  "$MERCURY_MODELS/SOURCE_INFO.txt" \
  "$MERCURY_MODELS/SHA256SUMS.txt"
