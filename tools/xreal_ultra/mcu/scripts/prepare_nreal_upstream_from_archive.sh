#!/usr/bin/env bash
set -euo pipefail

expand_tilde() {
  local value="$1"
  case "$value" in
    "~") printf '%s\n' "$HOME" ;;
    "~/"*) printf '%s\n' "$HOME/${value#"~/"}" ;;
    *) printf '%s\n' "$value" ;;
  esac
}

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_PROJECT="${ROOT_PROJECT:-$(cd "$SCRIPT_DIR/../../../.." && pwd)}"
ROOT_PROJECT="$(expand_tilde "$ROOT_PROJECT")"

NREAL_DRIVER_URL="${NREAL_DRIVER_URL:-https://gitlab.com/TheJackiMonster/nrealAirLinuxDriver.git}"
NREAL_DRIVER_REF="${NREAL_DRIVER_REF:-9a1f55c9838cf92627cde62f9bd69269d213d134}"
DEST_DIR="${NREAL_DRIVER_DIR:-$ROOT_PROJECT/third_party/nrealAirLinuxDriver}"
DEST_DIR="$(expand_tilde "$DEST_DIR")"
ARCHIVE_PATH="${1:-}"

usage() {
  cat >&2 <<EOF
Usage:
  $0
  $0 /path/to/nrealAirLinuxDriver.zip

Default mode downloads the reviewed upstream snapshot from GitLab and installs
it into third_party without a nested .git directory. Archive mode is kept for
offline refreshes and local snapshots.

Environment:
  ROOT_PROJECT=$ROOT_PROJECT
  NREAL_DRIVER_DIR=$DEST_DIR
  NREAL_DRIVER_URL=$NREAL_DRIVER_URL
  NREAL_DRIVER_REF=$NREAL_DRIVER_REF
EOF
}

validate_source_tree() {
  local src_dir="$1"
  if [[ ! -f "$src_dir/interface_lib/include/device_mcu.h" ]]; then
    echo "[prepare_nreal_upstream][ERROR] source tree does not look like full nrealAirLinuxDriver checkout" >&2
    echo "Expected: $src_dir/interface_lib/include/device_mcu.h" >&2
    exit 2
  fi
}

install_source_tree() {
  local src_dir="$1"
  local source_label="$2"

  validate_source_tree "$src_dir"

  mkdir -p "$(dirname "$DEST_DIR")"
  rm -rf "$DEST_DIR.tmp" "$DEST_DIR"
  mkdir -p "$DEST_DIR.tmp"

  # Do not keep the nested .git directory inside project archives/packages. This
  # is a reviewed upstream snapshot used only by explicit MCU build scripts.
  # The destination is under third_party/ by default and can be source-controlled.
  ( cd "$src_dir" && tar --exclude='.git' -cf - . ) | ( cd "$DEST_DIR.tmp" && tar -xf - )

  cat > "$DEST_DIR.tmp/README.XR_TRACKING_SNAPSHOT.md" <<EOF
# nrealAirLinuxDriver snapshot

This directory is a vendored snapshot prepared by:

    $SCRIPT_DIR/$(basename "$0")

Source:

    $source_label

Default upstream URL:

    $NREAL_DRIVER_URL

Pinned commit/ref:

    $NREAL_DRIVER_REF

This code is used only by explicit opt-in XREAL MCU tools. It is not used by
the default XR runtime pipeline.
EOF

  printf '%s\n' "$NREAL_DRIVER_REF" > "$DEST_DIR.tmp/.xr_tracking_nreal_ref"
  printf '%s\n' "$source_label" > "$DEST_DIR.tmp/.xr_tracking_nreal_source"

  mv "$DEST_DIR.tmp" "$DEST_DIR"

  cat <<EOF
[prepare_nreal_upstream] installed upstream snapshot:
  source: $source_label
  dest:   $DEST_DIR

Build MCU tools:
  tools/xreal_ultra/mcu/scripts/build_mcu_tools.sh
EOF
}

prepare_from_gitlab() {
  if ! command -v git >/dev/null 2>&1; then
    echo "[prepare_nreal_upstream][ERROR] git is required for default download mode" >&2
    exit 2
  fi

  if [[ "${FORCE_NREAL_REFRESH:-0}" != "1" \
        && -f "$DEST_DIR/interface_lib/include/device_mcu.h" \
        && -f "$DEST_DIR/.xr_tracking_nreal_ref" \
        && "$(cat "$DEST_DIR/.xr_tracking_nreal_ref" 2>/dev/null || true)" == "$NREAL_DRIVER_REF" ]]; then
    cat <<EOF
[prepare_nreal_upstream] upstream snapshot already installed:
  dest: $DEST_DIR
  ref:  $NREAL_DRIVER_REF

Set FORCE_NREAL_REFRESH=1 to refresh it.
EOF
    return 0
  fi

  local tmp_dir
  tmp_dir="$(mktemp -d)"
  cleanup_git() { rm -rf "$tmp_dir"; }
  trap cleanup_git EXIT

  local clone_dir="$tmp_dir/nrealAirLinuxDriver"

  echo "[prepare_nreal_upstream] fetching upstream snapshot" >&2
  echo "  url:  $NREAL_DRIVER_URL" >&2
  echo "  ref:  $NREAL_DRIVER_REF" >&2
  echo "  dest: $DEST_DIR" >&2

  git clone --no-checkout "$NREAL_DRIVER_URL" "$clone_dir" >&2
  if ! git -C "$clone_dir" fetch --depth 1 origin "$NREAL_DRIVER_REF" >&2; then
    echo "[prepare_nreal_upstream][WARN] shallow fetch by commit failed; using cloned refs" >&2
  fi
  git -C "$clone_dir" checkout --quiet --detach "$NREAL_DRIVER_REF" >&2

  install_source_tree "$clone_dir" "$NREAL_DRIVER_URL @ $NREAL_DRIVER_REF"
  rm -rf "$tmp_dir"
  trap - EXIT
}

prepare_from_archive() {
  local archive_path="$1"
  archive_path="$(expand_tilde "$archive_path")"

  if [[ ! -f "$archive_path" ]]; then
    echo "[prepare_nreal_upstream][ERROR] archive not found: $archive_path" >&2
    exit 2
  fi
  if ! command -v unzip >/dev/null 2>&1; then
    echo "[prepare_nreal_upstream][ERROR] unzip is required for archive mode" >&2
    exit 2
  fi

  local tmp_dir
  tmp_dir="$(mktemp -d)"
  cleanup_archive() { rm -rf "$tmp_dir"; }
  trap cleanup_archive EXIT

  unzip -q "$archive_path" -d "$tmp_dir"

  local src_dir=""
  if [[ -f "$tmp_dir/nrealAirLinuxDriver/CMakeLists.txt" ]]; then
    src_dir="$tmp_dir/nrealAirLinuxDriver"
  else
    local found
    found="$(find "$tmp_dir" -maxdepth 2 -type f -name CMakeLists.txt -printf '%h\n' | head -n 1 || true)"
    if [[ -n "$found" && -f "$found/interface_lib/include/device_mcu.h" ]]; then
      src_dir="$found"
    fi
  fi

  if [[ -z "$src_dir" ]]; then
    echo "[prepare_nreal_upstream][ERROR] archive does not look like full nrealAirLinuxDriver checkout" >&2
    echo "Expected interface_lib/include/device_mcu.h" >&2
    exit 2
  fi

  install_source_tree "$src_dir" "$archive_path"
  rm -rf "$tmp_dir"
  trap - EXIT
}

case "$ARCHIVE_PATH" in
  "" )
    prepare_from_gitlab
    ;;
  -h|--help )
    usage
    exit 0
    ;;
  * )
    prepare_from_archive "$ARCHIVE_PATH"
    ;;
esac
