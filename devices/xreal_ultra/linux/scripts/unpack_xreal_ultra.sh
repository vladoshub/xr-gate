#!/usr/bin/env bash
set -euo pipefail

log() { echo "[unpack_xr_gate_release] $*" >&2; }
fatal() { echo "[unpack_xr_gate_release][ERROR] $*" >&2; exit 1; }

usage() {
  cat <<'USAGE'
Usage:
  ./unpack_xreal_ultra.sh [options] [DEST_DIR]

Expected files by default are next to this script. Release tar.gz assets are
preferred, while old GitHub Actions artifact zip wrappers are still supported:
  xreal_ultra_linux_x64.tar.gz      or xreal-ultra-linux-x64.zip
  hand-tracking-models-mercury.tar.gz or hand-tracking-models-mercury.zip

Options:
  -d, --dest DIR          Destination directory where the runtime package will be extracted.
                          Default: directory containing this script.
  --main FILE             Main runtime archive: .tar.gz release asset or old GitHub artifact .zip.
  --main-archive FILE     Alias for --main.
  --main-zip FILE         Backward-compatible alias for --main.
  --models FILE           Mercury models archive: .tar.gz release asset or old GitHub artifact .zip.
  --models-archive FILE   Alias for --models.
  --models-zip FILE       Backward-compatible alias for --models.
  -f, --force             Remove existing extracted runtime package directory before extracting.
  --keep-tmp              Keep temporary extraction directory for debugging.
  -h, --help              Show this help.

Environment variables:
  DEST_DIR=/path          Same as --dest.
  MAIN_ARCHIVE=/path      Same as --main.
  MAIN_ZIP=/path/file.zip Backward-compatible alias for MAIN_ARCHIVE.
  MODELS_ARCHIVE=/path    Same as --models.
  MODELS_ZIP=/path.zip    Backward-compatible alias for MODELS_ARCHIVE.
  FORCE=1                Same as --force.
  KEEP_TMP=1             Same as --keep-tmp.

Examples:
  ./unpack_xreal_ultra.sh
  ./unpack_xreal_ultra.sh ~/xr-gate-release
  ./unpack_xreal_ultra.sh --dest ~/xr-gate-release
  ./unpack_xreal_ultra.sh --main ./xreal_ultra_linux_x64.tar.gz --models ./hand-tracking-models-mercury.tar.gz
  ./unpack_xreal_ultra.sh --main ./xreal-ultra-linux-x64.zip --models ./hand-tracking-models-mercury.zip
  FORCE=1 ./unpack_xreal_ultra.sh --dest ~/xr-gate-release
USAGE
}

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

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
    "$SCRIPT_DIR/xreal_ultra_linux_x64.tar.gz" \
    "$SCRIPT_DIR/xreal-ultra-linux-x64.tar.gz" \
    "$SCRIPT_DIR/xreal-ultra-linux-x64.zip")"
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
  'xreal_ultra_linux_x64.tar.gz' \
  'xreal-ultra-linux-x64.tar.gz')"

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