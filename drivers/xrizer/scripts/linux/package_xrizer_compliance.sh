#!/usr/bin/env bash
set -euo pipefail

log() { echo "[package_xrizer_compliance] $*" >&2; }
fail() { echo "[package_xrizer_compliance][ERROR] $*" >&2; exit 1; }

expand_path() {
  local path="$1"
  if [[ "$path" == "~" ]]; then
    printf '%s\n' "$HOME"
  elif [[ "$path" == "~/"* ]]; then
    printf '%s/%s\n' "$HOME" "${path#~/}"
  else
    printf '%s\n' "$path"
  fi
}

find_project_root() {
  local d
  d="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
  while [[ "$d" != "/" && -n "$d" ]]; do
    if [[ -d "$d/drivers/xrizer" && -d "$d/devices/common" ]]; then
      printf '%s\n' "$d"
      return 0
    fi
    d="$(dirname "$d")"
  done
  return 1
}

ROOT_PROJECT="$(expand_path "${ROOT_PROJECT:-${XR_ROOT_PROJECT:-$(find_project_root || true)}}")"
[[ -n "$ROOT_PROJECT" && -d "$ROOT_PROJECT" ]] || fail "cannot determine ROOT_PROJECT"

XRIZER_DIR="$(expand_path "${XRIZER_DIR:-$ROOT_PROJECT/third_party/xrizer}")"
INSTALL_BIN_DIR="$(expand_path "${INSTALL_BIN_DIR:-${XR_BIN_ROOT:-$ROOT_PROJECT/bin}/drivers/xrizer}")"
XRIZER_REPO="${XRIZER_REPO:-https://github.com/Supreeeme/xrizer.git}"
XRIZER_REF_REQUESTED="${XRIZER_REF_REQUESTED:-${XRIZER_REF:-unknown}}"
XRIZER_VENDOR_CARGO_SOURCES="${XRIZER_VENDOR_CARGO_SOURCES:-1}"
XRIZER_ALLOW_DIRTY_SOURCE="${XRIZER_ALLOW_DIRTY_SOURCE:-0}"
XRIZER_SOURCE_ARCHIVE_BASENAME="${XRIZER_SOURCE_ARCHIVE_BASENAME:-xrizer-corresponding-source}"

[[ -d "$XRIZER_DIR/.git" || -f "$XRIZER_DIR/.git" ]] || fail "xrizer Git checkout not found: $XRIZER_DIR"
command -v git >/dev/null 2>&1 || fail "git not found"
command -v rsync >/dev/null 2>&1 || fail "rsync not found"
command -v tar >/dev/null 2>&1 || fail "tar not found"
command -v gzip >/dev/null 2>&1 || fail "gzip not found"
command -v sha256sum >/dev/null 2>&1 || fail "sha256sum not found"

resolved_commit="$(git -C "$XRIZER_DIR" rev-parse HEAD)"
short_commit="$(git -C "$XRIZER_DIR" rev-parse --short=12 HEAD)"
commit_timestamp="$(git -C "$XRIZER_DIR" show -s --format=%ct HEAD)"
source_dir_name="xrizer-source-$short_commit"
archive_name="${XRIZER_SOURCE_ARCHIVE_BASENAME}-${resolved_commit}.tar.gz"

tracked_status="$(git -C "$XRIZER_DIR" status --porcelain --untracked-files=no)"
if [[ -n "$tracked_status" && "$XRIZER_ALLOW_DIRTY_SOURCE" != "1" ]]; then
  printf '%s\n' "$tracked_status" >&2
  fail "xrizer checkout has tracked modifications; refusing to create ambiguous Corresponding Source. Set XRIZER_ALLOW_DIRTY_SOURCE=1 only when intentionally packaging those modifications."
fi

license_file=""
for candidate in \
  "$XRIZER_DIR/LICENSE" \
  "$XRIZER_DIR/LICENSE.txt" \
  "$XRIZER_DIR/LICENSE.md" \
  "$XRIZER_DIR/COPYING" \
  "$XRIZER_DIR/COPYING.txt"; do
  if [[ -f "$candidate" ]]; then
    license_file="$candidate"
    break
  fi
done
[[ -n "$license_file" ]] || fail "upstream xrizer license file not found under $XRIZER_DIR"

tmp_dir="$(mktemp -d)"
cleanup() { rm -rf "$tmp_dir"; }
trap cleanup EXIT
source_root="$tmp_dir/$source_dir_name"
mkdir -p "$source_root"

log "copying xrizer source tree at $resolved_commit"
rsync -a \
  --exclude='.git' \
  --exclude='.git/' \
  --exclude='target/' \
  --exclude='build/' \
  --exclude='__pycache__/' \
  --exclude='*.pyc' \
  "$XRIZER_DIR/" "$source_root/"

mkdir -p "$source_root/xr-gate-integration"
install -m 0755 \
  "$ROOT_PROJECT/drivers/xrizer/scripts/linux/install_xrizer.sh" \
  "$source_root/xr-gate-integration/install_xrizer.sh"
install -m 0755 \
  "$ROOT_PROJECT/drivers/xrizer/scripts/linux/package_xrizer_compliance.sh" \
  "$source_root/xr-gate-integration/package_xrizer_compliance.sh"
if [[ -f "$ROOT_PROJECT/drivers/xrizer/README.md" ]]; then
  install -m 0644 "$ROOT_PROJECT/drivers/xrizer/README.md" "$source_root/xr-gate-integration/README.md"
fi

submodule_status="$(git -C "$XRIZER_DIR" submodule status --recursive 2>/dev/null || true)"
cat > "$source_root/SOURCE_INFO.txt" <<EOF_INFO
Component: xrizer
License: GPL-3.0-or-later
Upstream repository: $XRIZER_REPO
Requested ref: $XRIZER_REF_REQUESTED
Resolved commit: $resolved_commit
Source commit timestamp: $commit_timestamp
Tracked source modifications included: $([[ -n "$tracked_status" ]] && printf 'yes' || printf 'no')
Cargo dependency sources vendored: $([[ "$XRIZER_VENDOR_CARGO_SOURCES" == "1" ]] && printf 'yes' || printf 'no')

Git submodules:
${submodule_status:-  none}
EOF_INFO

cat > "$source_root/BUILDING_XRIZER.md" <<'EOF_BUILD'
# Building this xrizer source package

This archive contains the xrizer source used for the accompanying binary,
including initialized Git submodule working trees. When `vendor/` and
`.cargo/config.toml` are present, Cargo dependencies are resolved from the
included vendored sources.

A direct upstream-style build is:

```bash
cargo xbuild --locked --release
```

The XR Gate integration build entrypoint is:

```bash
ROOT_PROJECT=/path/to/xr-gate \
XRIZER_DIR=/path/to/this/source \
CLONE_XRIZER=0 \
XRIZER_PACKAGE_COMPLIANCE=0 \
/path/to/xr-gate/drivers/xrizer/scripts/linux/install_xrizer.sh
```

`install_xrizer.sh` recognizes this archive as a source snapshot and does not
require `.git` metadata when `CLONE_XRIZER=0`.

The original integration scripts used by the build are retained under
`xr-gate-integration/`. System compiler, Vulkan/shader compiler, Rust toolchain,
and other documented build prerequisites are not bundled.
EOF_BUILD

if [[ "$XRIZER_VENDOR_CARGO_SOURCES" == "1" ]]; then
  command -v cargo >/dev/null 2>&1 || fail "cargo not found; cannot vendor Cargo dependency sources"
  [[ -f "$XRIZER_DIR/Cargo.lock" ]] || fail "Cargo.lock missing; cannot create reproducible vendored source package"
  mkdir -p "$source_root/.cargo"
  log "vendoring Cargo dependency sources"
  (
    cd "$XRIZER_DIR"
    cargo vendor --locked --versioned-dirs "$source_root/vendor"
  ) > "$source_root/.cargo/config.toml"
  sed -i -E 's#^directory = ".*"$#directory = "vendor"#' \
    "$source_root/.cargo/config.toml"
fi

mkdir -p "$INSTALL_BIN_DIR/source"
install -m 0644 "$license_file" "$INSTALL_BIN_DIR/LICENSE.GPL-3.0-or-later"

archive_path="$INSTALL_BIN_DIR/source/$archive_name"
log "creating Corresponding Source archive: $archive_path"
(
  cd "$tmp_dir"
  tar \
    --sort=name \
    --mtime="@$commit_timestamp" \
    --owner=0 --group=0 --numeric-owner \
    -cf - "$source_dir_name" | gzip -n > "$archive_path"
)

cat > "$INSTALL_BIN_DIR/SOURCE.txt" <<EOF_SOURCE
Component: xrizer
License: GPL-3.0-or-later
Upstream repository: $XRIZER_REPO
Requested ref: $XRIZER_REF_REQUESTED
Resolved commit: $resolved_commit
Corresponding Source archive: source/$archive_name
Cargo dependency sources vendored: $([[ "$XRIZER_VENDOR_CARGO_SOURCES" == "1" ]] && printf 'yes' || printf 'no')
Build integration scripts are included inside the source archive under xr-gate-integration/.
EOF_SOURCE

(
  cd "$INSTALL_BIN_DIR"
  sha256sum "LICENSE.GPL-3.0-or-later" "source/$archive_name" > SHA256SUMS.txt
)

log "GPL license: $INSTALL_BIN_DIR/LICENSE.GPL-3.0-or-later"
log "source metadata: $INSTALL_BIN_DIR/SOURCE.txt"
log "Corresponding Source: $archive_path"
