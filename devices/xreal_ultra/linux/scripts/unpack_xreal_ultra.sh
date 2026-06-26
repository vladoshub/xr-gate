#!/usr/bin/env bash
set -euo pipefail

log() { echo "[unpack_xr_gate_release] $*" >&2; }
fatal() { echo "[unpack_xr_gate_release][ERROR] $*" >&2; exit 1; }

usage() {
  cat <<'USAGE'
Usage:
  ./unpack_xreal_ultra.sh [options] [DEST_DIR]

Expected files by default are next to this script:
  xreal-ultra-linux-x64.zip
  hand-tracking-models-mercury.zip

Options:
  -d, --dest DIR          Destination directory where the runtime package will be extracted.
                          Default: directory containing this script.
  --main-zip FILE         Main GitHub artifact zip.
                          Default: ./xreal-ultra-linux-x64.zip next to this script.
  --models-zip FILE       Mercury models GitHub artifact zip.
                          Default: ./hand-tracking-models-mercury.zip next to this script.
  -f, --force             Remove existing extracted runtime package directory before extracting.
  --keep-tmp              Keep temporary extraction directory for debugging.
  -h, --help              Show this help.

Environment variables:
  DEST_DIR=/path          Same as --dest.
  MAIN_ZIP=/path/file.zip Same as --main-zip.
  MODELS_ZIP=/path.zip    Same as --models-zip.
  FORCE=1                Same as --force.
  KEEP_TMP=1             Same as --keep-tmp.

Examples:
  ./unpack_xreal_ultra.sh
  ./unpack_xreal_ultra.sh ~/xr-gate-release
  ./unpack_xreal_ultra.sh --dest ~/xr-gate-release
  FORCE=1 ./unpack_xreal_ultra.sh --dest ~/xr-gate-release
USAGE
}

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

MAIN_ZIP="${MAIN_ZIP:-$SCRIPT_DIR/xreal-ultra-linux-x64.zip}"
MODELS_ZIP="${MODELS_ZIP:-$SCRIPT_DIR/hand-tracking-models-mercury.zip}"
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
    --main-zip)
      [[ $# -ge 2 ]] || fatal "$1 requires a file argument"
      MAIN_ZIP="$2"
      shift 2
      ;;
    --models-zip)
      [[ $# -ge 2 ]] || fatal "$1 requires a file argument"
      MODELS_ZIP="$2"
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
    -*)
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
MAIN_ZIP="${MAIN_ZIP/#\~/$HOME}"
MODELS_ZIP="${MODELS_ZIP/#\~/$HOME}"
DEST_DIR="${DEST_DIR/#\~/$HOME}"

command -v unzip >/dev/null 2>&1 || fatal "unzip not found. Install it: sudo apt install unzip"
command -v tar >/dev/null 2>&1 || fatal "tar not found"
command -v find >/dev/null 2>&1 || fatal "find not found"
command -v mktemp >/dev/null 2>&1 || fatal "mktemp not found"
command -v cp >/dev/null 2>&1 || fatal "cp not found"

[[ -f "$MAIN_ZIP" ]] || fatal "main artifact zip not found: $MAIN_ZIP"
[[ -f "$MODELS_ZIP" ]] || fatal "models artifact zip not found: $MODELS_ZIP"

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

log "main artifact: $MAIN_ZIP"
log "models artifact: $MODELS_ZIP"
log "destination: $DEST_DIR"

mkdir -p "$TMP_DIR/main_zip" "$TMP_DIR/models_zip" "$TMP_DIR/main_probe" "$TMP_DIR/models_extract"

log "unzip main GitHub artifact wrapper"
unzip -q "$MAIN_ZIP" -d "$TMP_DIR/main_zip"

log "unzip models GitHub artifact wrapper"
unzip -q "$MODELS_ZIP" -d "$TMP_DIR/models_zip"

mapfile -t MAIN_TARS < <(
  find "$TMP_DIR/main_zip" -type f \( \
    -name 'xreal_ultra_linux_x64.tar.gz' -o \
    -name 'xreal-ultra-linux-x64.tar.gz' \
  \) | sort
)

mapfile -t MODELS_TARS < <(
  find "$TMP_DIR/models_zip" -type f -name 'hand-tracking-models-mercury.tar.gz' | sort
)

[[ "${#MAIN_TARS[@]}" -eq 1 ]] || {
  find "$TMP_DIR/main_zip" -maxdepth 5 -type f -print >&2 || true
  fatal "expected exactly one xreal_ultra_linux_x64.tar.gz inside $MAIN_ZIP, found ${#MAIN_TARS[@]}"
}

[[ "${#MODELS_TARS[@]}" -eq 1 ]] || {
  find "$TMP_DIR/models_zip" -maxdepth 5 -type f -print >&2 || true
  fatal "expected exactly one hand-tracking-models-mercury.tar.gz inside $MODELS_ZIP, found ${#MODELS_TARS[@]}"
}

MAIN_TAR="${MAIN_TARS[0]}"
MODELS_TAR="${MODELS_TARS[0]}"

log "main tar: $MAIN_TAR"
log "models tar: $MODELS_TAR"

log "probe main runtime package layout"
tar -C "$TMP_DIR/main_probe" -xzf "$MAIN_TAR"

detect_package_root() {
  local root="$1"
  local d

  while IFS= read -r -d '' d; do
    if [[ -d "$d/bin" && -d "$d/devices/xreal_ultra" ]]; then
      printf '%s\n' "$d"
      return 0
    fi
  done < <(find "$root" -type d -print0)

  return 1
}

SRC_PACKAGE_DIR="$(detect_package_root "$TMP_DIR/main_probe" || true)"
[[ -n "${SRC_PACKAGE_DIR:-}" ]] || {
  log "main tar content preview:"
  tar -tzf "$MAIN_TAR" | head -120 >&2 || true
  fatal "could not detect package root. Expected directory containing bin/ and devices/xreal_ultra/"
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
[[ -d "$XR_PACKAGE_DIR/devices/xreal_ultra" ]] || fatal "extracted package missing devices/xreal_ultra"

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
echo "  ./devices/xreal_ultra/linux/scripts/install_runtime_deps_ubuntu24.sh"
