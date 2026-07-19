#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
_HELPER_NAME="$(basename "$0")"
_DETECTED_TARGET=""
if [[ "$_HELPER_NAME" =~ ^unpack_(.+)\.sh$ ]]; then
  _DETECTED_TARGET="${BASH_REMATCH[1]}"
fi
XR_RELEASE_DEVICE_TARGET="${XR_RELEASE_DEVICE_TARGET:-${_DETECTED_TARGET:-generic}}"
XR_RELEASE_DEVICE_DISPLAY_NAME="${XR_RELEASE_DEVICE_DISPLAY_NAME:-$XR_RELEASE_DEVICE_TARGET}"
XR_RELEASE_DEVICE_ENV_NAME="${XR_RELEASE_DEVICE_ENV_NAME:-$XR_RELEASE_DEVICE_TARGET.env}"
XR_RELEASE_REQUIRE_DEVICE="${XR_RELEASE_REQUIRE_DEVICE:-1}"
XR_RELEASE_TARGET_HYPHEN="${XR_RELEASE_DEVICE_TARGET//_/-}"

log() { echo "[unpack_device_release:$XR_RELEASE_DEVICE_TARGET] $*" >&2; }
fatal() { echo "[unpack_device_release:$XR_RELEASE_DEVICE_TARGET][ERROR] $*" >&2; exit 1; }

usage() {
  cat <<USAGE
Extract the $XR_RELEASE_DEVICE_DISPLAY_NAME runtime package and install the
separately distributed Mercury hand-tracking models.

Usage:
  $0 [options] [DEST_DIR]

Options:
  -d, --dest DIR       Destination parent directory.
  --main ARCHIVE       Main runtime .tar.gz/.tgz or GitHub artifact .zip.
  --models ARCHIVE     Mercury model .tar.gz or GitHub artifact .zip.
  -f, --force          Replace an existing extracted package.
  --keep-tmp           Keep temporary extraction directory.
  -h, --help           Show this help.

Environment:
  XR_RELEASE_DEVICE_TARGET       Device directory name. Current: $XR_RELEASE_DEVICE_TARGET
  XR_RELEASE_REQUIRE_DEVICE      If 1, require devices/<target>. Current: $XR_RELEASE_REQUIRE_DEVICE
  MAIN_ARCHIVE / MAIN_ZIP        Main archive override.
  MODELS_ARCHIVE / MODELS_ZIP    Models archive override.
  DEST_DIR, FORCE, KEEP_TMP       Equivalent option defaults.
USAGE
}

first_existing() {
  local candidate
  for candidate in "$@"; do
    candidate="${candidate/#\~/$HOME}"
    if [[ -f "$candidate" ]]; then
      printf '%s\n' "$candidate"
      return 0
    fi
  done
  printf '%s\n' "${1/#\~/$HOME}"
}

MAIN_ARCHIVE="${MAIN_ARCHIVE:-${MAIN_ZIP:-}}"
if [[ -z "$MAIN_ARCHIVE" ]]; then
  MAIN_ARCHIVE="$(first_existing \
    "$SCRIPT_DIR/${XR_RELEASE_DEVICE_TARGET}_linux_x64.tar.gz" \
    "$SCRIPT_DIR/${XR_RELEASE_TARGET_HYPHEN}-linux-x64.tar.gz" \
    "$SCRIPT_DIR/${XR_RELEASE_TARGET_HYPHEN}-linux-x64.zip")"
fi

MODELS_ARCHIVE="${MODELS_ARCHIVE:-${MODELS_ZIP:-}}"
if [[ -z "$MODELS_ARCHIVE" ]]; then
  MODELS_ARCHIVE="$(first_existing \
    "$SCRIPT_DIR/hand-tracking-models-mercury.tar.gz" \
    "$SCRIPT_DIR/hand-tracking-models-mercury.zip")"
fi

DEST_DIR="${DEST_DIR:-$SCRIPT_DIR}"
FORCE="${FORCE:-0}"
KEEP_TMP="${KEEP_TMP:-0}"

POSITIONAL_DEST=""

while [[ $# -gt 0 ]]; do
  case "$1" in
    -d|--dest)
      [[ $# -ge 2 ]] || fatal "$1 requires a directory argument"
      DEST_DIR="$2"
      shift 2
      ;;
    --main|--main-archive|--main-zip)
      [[ $# -ge 2 ]] || fatal "$1 requires a file argument"
      MAIN_ARCHIVE="$2"
      shift 2
      ;;
    --models|--models-archive|--models-zip)
      [[ $# -ge 2 ]] || fatal "$1 requires a file argument"
      MODELS_ARCHIVE="$2"
      shift 2
      ;;
    -f|--force)
      FORCE=1
      shift
      ;;
    --keep-tmp)
      KEEP_TMP=1
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    --)
      shift
      break
      ;;
    -* )
      fatal "unknown option: $1"
      ;;
    *)
      if [[ -n "$POSITIONAL_DEST" ]]; then
        fatal "unexpected extra positional argument: $1"
      fi
      POSITIONAL_DEST="$1"
      shift
      ;;
  esac
done

if [[ $# -gt 0 ]]; then
  if [[ -n "$POSITIONAL_DEST" || $# -gt 1 ]]; then
    fatal "unexpected positional arguments: $*"
  fi
  POSITIONAL_DEST="$1"
fi

if [[ -n "$POSITIONAL_DEST" ]]; then
  DEST_DIR="$POSITIONAL_DEST"
fi

# Expand a leading ~/ manually; quoted shell variables do not expand it.
MAIN_ARCHIVE="${MAIN_ARCHIVE/#\~/$HOME}"
MODELS_ARCHIVE="${MODELS_ARCHIVE/#\~/$HOME}"
DEST_DIR="${DEST_DIR/#\~/$HOME}"

command -v tar >/dev/null 2>&1 || fatal "tar not found"
command -v find >/dev/null 2>&1 || fatal "find not found"
command -v mktemp >/dev/null 2>&1 || fatal "mktemp not found"
command -v cp >/dev/null 2>&1 || fatal "cp not found"

[[ -f "$MAIN_ARCHIVE" ]] || fatal "main archive not found: $MAIN_ARCHIVE"
[[ -f "$MODELS_ARCHIVE" ]] || fatal "models archive not found: $MODELS_ARCHIVE"

mkdir -p "$DEST_DIR"
DEST_DIR="$(cd "$DEST_DIR" && pwd)"

TMP_DIR="$(mktemp -d)"
cleanup() {
  if [[ "$KEEP_TMP" != "1" ]]; then
    rm -rf "$TMP_DIR"
  else
    log "kept temp dir: $TMP_DIR"
  fi
}
trap cleanup EXIT

log "main archive: $MAIN_ARCHIVE"
log "models archive: $MODELS_ARCHIVE"
log "destination: $DEST_DIR"

mkdir -p "$TMP_DIR/main_zip" "$TMP_DIR/models_zip" "$TMP_DIR/main_probe" "$TMP_DIR/models_extract"

select_tar_from_archive() {
  local archive="$1"
  local extract_dir="$2"
  local label="$3"
  shift 3
  local -a tar_names=("$@")
  local tar_name

  case "$archive" in
    *.tar.gz|*.tgz)
      printf '%s\n' "$archive"
      return 0
      ;;
    *.zip)
      command -v unzip >/dev/null 2>&1 || fatal "unzip not found. Install it: sudo apt install unzip"
      log "unzip $label GitHub artifact wrapper"
      unzip -q "$archive" -d "$extract_dir"
      local -a find_args=()
      for tar_name in "${tar_names[@]}"; do
        if [[ "${#find_args[@]}" -gt 0 ]]; then
          find_args+=( -o )
        fi
        find_args+=( -name "$tar_name" )
      done
      local -a matches=()
      mapfile -t matches < <(find "$extract_dir" -type f \( "${find_args[@]}" \) | sort)
      [[ "${#matches[@]}" -eq 1 ]] || {
        find "$extract_dir" -maxdepth 5 -type f -print >&2 || true
        fatal "expected exactly one ${tar_names[*]} inside $archive, found ${#matches[@]}"
      }
      printf '%s\n' "${matches[0]}"
      return 0
      ;;
    *)
      fatal "unsupported $label archive type: $archive. Expected .tar.gz/.tgz or .zip"
      ;;
  esac
}

MAIN_TAR="$(select_tar_from_archive \
  "$MAIN_ARCHIVE" "$TMP_DIR/main_zip" "main" \
  "${XR_RELEASE_DEVICE_TARGET}_linux_x64.tar.gz" \
  "${XR_RELEASE_TARGET_HYPHEN}-linux-x64.tar.gz")"

MODELS_TAR="$(select_tar_from_archive \
  "$MODELS_ARCHIVE" "$TMP_DIR/models_zip" "models" \
  'hand-tracking-models-mercury.tar.gz')"

log "main tar: $MAIN_TAR"
log "models tar: $MODELS_TAR"

log "probe main runtime package layout"
tar -C "$TMP_DIR/main_probe" -xzf "$MAIN_TAR"

detect_package_root() {
  local root="$1"
  local d

  while IFS= read -r -d '' d; do
    [[ -d "$d/bin" && -d "$d/devices/common" ]] || continue
    if [[ "$XR_RELEASE_REQUIRE_DEVICE" == "1" && ! -d "$d/devices/$XR_RELEASE_DEVICE_TARGET" ]]; then
      continue
    fi
    printf '%s\n' "$d"
    return 0
  done < <(find "$root" -type d -print0)

  return 1
}

SRC_PACKAGE_DIR="$(detect_package_root "$TMP_DIR/main_probe" || true)"
[[ -n "${SRC_PACKAGE_DIR:-}" ]] || {
  log "main tar content preview:"
  tar -tzf "$MAIN_TAR" | head -120 >&2 || true
  fatal "could not detect package root. Expected directory containing bin/ and devices/common/"
}

REL_PACKAGE_DIR="${SRC_PACKAGE_DIR#"$TMP_DIR/main_probe"/}"
XR_PACKAGE_DIR="$DEST_DIR/$REL_PACKAGE_DIR"

log "detected package root inside tar: $REL_PACKAGE_DIR"

if [[ -e "$XR_PACKAGE_DIR" && "$FORCE" != "1" ]]; then
  fatal "destination package already exists: $XR_PACKAGE_DIR
Set FORCE=1 or pass --force to overwrite:
  FORCE=1 $0 --dest \"$DEST_DIR\"
  $0 --force --dest \"$DEST_DIR\""
fi

if [[ -e "$XR_PACKAGE_DIR" && "$FORCE" == "1" ]]; then
  log "remove existing package directory: $XR_PACKAGE_DIR"
  rm -rf "$XR_PACKAGE_DIR"
fi

log "extract main runtime package into: $DEST_DIR"
tar -C "$DEST_DIR" -xzf "$MAIN_TAR"

[[ -d "$XR_PACKAGE_DIR/bin" ]] || fatal "extracted package missing bin directory: $XR_PACKAGE_DIR/bin"
[[ -d "$XR_PACKAGE_DIR/devices/common" ]] || fatal "extracted package missing devices/common"
if [[ "$XR_RELEASE_REQUIRE_DEVICE" == "1" ]]; then
  [[ -d "$XR_PACKAGE_DIR/devices/$XR_RELEASE_DEVICE_TARGET" ]] || fatal "extracted package missing devices/$XR_RELEASE_DEVICE_TARGET"
fi

log "extract Mercury hand-tracking models to temp"
tar -C "$TMP_DIR/models_extract" -xzf "$MODELS_TAR"

mapfile -t DETECT_MODELS < <(
  find "$TMP_DIR/models_extract" -type f -name 'grayscale_detection_160x160.onnx' | sort
)

[[ "${#DETECT_MODELS[@]}" -eq 1 ]] || {
  log "models tar content preview:"
  tar -tzf "$MODELS_TAR" | head -120 >&2 || true
  fatal "expected exactly one grayscale_detection_160x160.onnx in models tar, found ${#DETECT_MODELS[@]}"
}

SRC_MERCURY_DIR="$(dirname "${DETECT_MODELS[0]}")"

[[ -f "$SRC_MERCURY_DIR/grayscale_keypoint_jan18.onnx" ]] || {
  log "detected Mercury dir: $SRC_MERCURY_DIR"
  fatal "missing grayscale_keypoint_jan18.onnx near detection model"
}

DST_MERCURY_DIR="$XR_PACKAGE_DIR/bin/hand-tracking-models/mercury"

log "install Mercury hand-tracking models"
log "  from: $SRC_MERCURY_DIR"
log "  to:   $DST_MERCURY_DIR"

rm -rf "$DST_MERCURY_DIR"
mkdir -p "$DST_MERCURY_DIR"
cp -a "$SRC_MERCURY_DIR/." "$DST_MERCURY_DIR/"

DETECT_MODEL="$DST_MERCURY_DIR/grayscale_detection_160x160.onnx"
KEYPOINT_MODEL="$DST_MERCURY_DIR/grayscale_keypoint_jan18.onnx"

[[ -f "$DETECT_MODEL" ]] || fatal "missing detection model after extraction: $DETECT_MODEL"
[[ -f "$KEYPOINT_MODEL" ]] || fatal "missing keypoint model after extraction: $KEYPOINT_MODEL"

log "OK"
echo
echo "Extracted runtime package:"
echo "  $XR_PACKAGE_DIR"
echo
echo "Installed Mercury models:"
ls -lh "$DST_MERCURY_DIR"
echo
echo "Next:"
echo "  cd \"$XR_PACKAGE_DIR\""
echo "  ./devices/common/linux/scripts/runtime/install_runtime_deps_ubuntu24.sh"