#!/usr/bin/env bash
set -euo pipefail

_dependency_support="$(dirname "${BASH_SOURCE[0]}")"
if [[ -f "$_dependency_support/dependency_policy.sh" ]]; then
  source "$_dependency_support/dependency_policy.sh"  # Corresponding Source archive
else
  source "$_dependency_support/../../../../devices/common/linux/scripts/build/dependency_policy.sh"
fi
unset _dependency_support

log() { echo "[install_xrizer] $*" >&2; }
fail() { echo "[install_xrizer][ERROR] $*" >&2; exit 1; }

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
[[ -n "$ROOT_PROJECT" && -d "$ROOT_PROJECT" ]] || fail "cannot determine ROOT_PROJECT. Set ROOT_PROJECT=/home/vlados/src/xr_tracking"

THIRD_PARTY_DIR="$(expand_path "${THIRD_PARTY_DIR:-$ROOT_PROJECT/third_party}")"
XRIZER_DIR="$(expand_path "${XRIZER_DIR:-$THIRD_PARTY_DIR/xrizer}")"
XRIZER_REPO="${XRIZER_REPO:-https://github.com/Supreeeme/xrizer.git}"
XRIZER_REF="${XRIZER_REF:-31319560c1bd0f1e5c16936a946bb1c7295dbfd9}"
INSTALL_BIN_DIR="$(expand_path "${INSTALL_BIN_DIR:-${XR_BIN_ROOT:-$ROOT_PROJECT/bin}/drivers/xrizer}")"
XRIZER_BUILD_PROFILE="${XRIZER_BUILD_PROFILE:-release}"
CLONE_XRIZER="${CLONE_XRIZER:-1}"
XRIZER_INSTALL_RUSTUP="${XRIZER_INSTALL_RUSTUP:-0}"
XRIZER_INSTALL_CARGO_XBUILD="${XRIZER_INSTALL_CARGO_XBUILD:-1}"
XRIZER_INSTALL_RUST_SRC="${XRIZER_INSTALL_RUST_SRC:-1}"
XRIZER_PACKAGE_COMPLIANCE="${XRIZER_PACKAGE_COMPLIANCE:-1}"
XRIZER_VENDOR_CARGO_SOURCES="${XRIZER_VENDOR_CARGO_SOURCES:-1}"
RUSTUP_INIT_URL="${RUSTUP_INIT_URL:-https://sh.rustup.rs}"

case "$XRIZER_BUILD_PROFILE" in
  release|debug) ;;
  *) fail "unsupported XRIZER_BUILD_PROFILE=$XRIZER_BUILD_PROFILE; expected release or debug" ;;
esac

mkdir -p "$THIRD_PARTY_DIR" "$(dirname "$INSTALL_BIN_DIR")"

if [[ ! -e "$XRIZER_DIR/.git" && "$CLONE_XRIZER" == 0 && -f "$XRIZER_DIR/Cargo.toml" ]]; then
  log "using xrizer source snapshot without Git metadata: $XRIZER_DIR"
else
  XRIZER_CHECKOUT_REF="$(xr_ensure_git_source "$XRIZER_DIR" "$XRIZER_REPO" "$XRIZER_REF" "$CLONE_XRIZER")" || exit $?
  git -C "$XRIZER_DIR" checkout "$XRIZER_CHECKOUT_REF"
  xr_update_submodules "$XRIZER_DIR"
fi
# Toolchain files are relative to the upstream checkout. Checking/installing
# cargo-xbuild and rust-src from another working directory can select the wrong
# Rust version, leaving the real build with an unprepared toolchain offline.
cd "$XRIZER_DIR"


find_libclang_dir() {
  local candidate
  for candidate in "${LIBCLANG_PATH:-}" /usr/lib/llvm-*/lib /usr/lib/x86_64-linux-gnu /usr/lib; do
    if [[ -n "$candidate" ]] && compgen -G "$candidate/libclang.so*" >/dev/null; then
      printf '%s\n' "$candidate"
      return 0
    fi
  done
  return 1
}

ensure_system_deps() {
  local missing=()

  command -v gcc >/dev/null 2>&1 || missing+=(build-essential)
  command -v pkg-config >/dev/null 2>&1 || missing+=(pkg-config)
  command -v clang >/dev/null 2>&1 || missing+=(clang)
  command -v glslc >/dev/null 2>&1 || missing+=(glslc)

  if ! find_libclang_dir >/dev/null &&
     ! ldconfig -p 2>/dev/null | grep -E 'libclang(-[0-9]+)?\.so' >/dev/null; then
    missing+=(libclang-dev)
  fi

  if [[ ${#missing[@]} -eq 0 ]]; then
    return 0
  fi

  fail "missing system build dependencies: ${missing[*]}. Run XR_BUILD_ONLY=xrizer ./devices/common/linux/scripts/build/install_build_deps_ubuntu.sh before building."
}

configure_clang_bindgen_env() {
  if [[ -z "${LIBCLANG_PATH:-}" ]]; then
    local libclang_dir
    if libclang_dir="$(find_libclang_dir)"; then
      export LIBCLANG_PATH="$libclang_dir"
      log "using LIBCLANG_PATH=$LIBCLANG_PATH"
    else
      log "warning: libclang.so not found in common paths; bindgen may fail"
    fi
  fi

  if [[ -z "${BINDGEN_EXTRA_CLANG_ARGS:-}" ]] && command -v gcc >/dev/null 2>&1; then
    local gcc_include multiarch args
    gcc_include="$(gcc -print-file-name=include 2>/dev/null || true)"
    multiarch="$(gcc -print-multiarch 2>/dev/null || true)"
    args=()
    [[ -n "$gcc_include" && -d "$gcc_include" ]] && args+=("-I$gcc_include")
    [[ -n "$multiarch" && -d "/usr/include/$multiarch" ]] && args+=("-I/usr/include/$multiarch")
    [[ -d /usr/include ]] && args+=("-I/usr/include")
    if [[ ${#args[@]} -gt 0 ]]; then
      export BINDGEN_EXTRA_CLANG_ARGS="${args[*]}"
      log "using BINDGEN_EXTRA_CLANG_ARGS=$BINDGEN_EXTRA_CLANG_ARGS"
    fi
  fi
}

ensure_rust_toolchain() {
  if command -v cargo >/dev/null 2>&1; then
    return 0
  fi

  if [[ "$XRIZER_INSTALL_RUSTUP" != "1" ]]; then
    fail "cargo not found. Install Rust toolchain first, or rerun with XRIZER_INSTALL_RUSTUP=1 to install rustup/cargo for this user."
  fi

  xr_require_network "Rust toolchain missing for xrizer"
  command -v curl >/dev/null 2>&1 || fail "curl not found; cannot install rustup. Install curl or install cargo manually."

  log "cargo not found; installing rustup/cargo for current user (XRIZER_INSTALL_RUSTUP=1)"
  curl --proto '=https' --tlsv1.2 -sSf "$RUSTUP_INIT_URL" | sh -s -- -y --profile minimal

  if [[ -f "$HOME/.cargo/env" ]]; then
    # shellcheck disable=SC1091
    source "$HOME/.cargo/env"
  else
    export PATH="$HOME/.cargo/bin:$PATH"
  fi

  command -v cargo >/dev/null 2>&1 || fail "cargo still not found after rustup install"
}

ensure_cargo_xbuild() {
  if cargo xbuild --version >/dev/null 2>&1; then
    return 0
  fi

  if [[ "$XRIZER_INSTALL_CARGO_XBUILD" != "1" ]]; then
    fail "cargo-xbuild is not installed. Install it manually or rerun with XRIZER_INSTALL_CARGO_XBUILD=1."
  fi

  xr_require_network "cargo-xbuild missing"
  log "cargo-xbuild not found; installing cargo-xbuild"
  cargo install cargo-xbuild --locked

  cargo xbuild --version >/dev/null 2>&1 || fail "cargo xbuild still not available after cargo-xbuild install"
}

ensure_rust_src() {
  if [[ "$XRIZER_INSTALL_RUST_SRC" != "1" ]]; then
    return 0
  fi

  local sysroot
  sysroot="$(rustc --print sysroot)" || fail "Rust toolchain missing for xrizer; prepare it online first"
  if [[ -f "$sysroot/lib/rustlib/src/rust/library/Cargo.toml" ]]; then
    return 0
  fi
  xr_require_network "rust-src missing in $sysroot"
  command -v rustup >/dev/null 2>&1 || fail "rust-src missing and rustup not installed"
  rustup component add rust-src

}

ensure_system_deps
configure_clang_bindgen_env
ensure_rust_toolchain
ensure_rust_src
ensure_cargo_xbuild

log "using glslc: $(command -v glslc)"
log "using clang: $(command -v clang)"
log "using cargo: $(command -v cargo)"
log "building xrizer ($XRIZER_BUILD_PROFILE)"
if [[ "$XRIZER_BUILD_PROFILE" == "release" ]]; then
  (cd "$XRIZER_DIR" && cargo xbuild --locked --release)
  BUILD_OUT="$XRIZER_DIR/target/release"
else
  (cd "$XRIZER_DIR" && cargo xbuild --locked)
  BUILD_OUT="$XRIZER_DIR/target/debug"
fi

[[ -d "$BUILD_OUT" ]] || fail "xrizer build output missing: $BUILD_OUT"

rm -rf "$INSTALL_BIN_DIR"
mkdir -p "$INSTALL_BIN_DIR"

log "installing xrizer runtime: $BUILD_OUT -> $INSTALL_BIN_DIR/runtime"
rsync -a --delete \
  --exclude='.fingerprint/' \
  --exclude='build/' \
  --exclude='deps/*.d' \
  --exclude='examples/' \
  --exclude='incremental/' \
  "$BUILD_OUT/" "$INSTALL_BIN_DIR/runtime/"

cat > "$INSTALL_BIN_DIR/env.sh" <<EOF_ENV
#!/usr/bin/env bash
set -euo pipefail
export XRIZER_RUNTIME_DIR="\${XRIZER_RUNTIME_DIR:-$INSTALL_BIN_DIR/runtime}"
export VR_OVERRIDE="\${VR_OVERRIDE:-\$XRIZER_RUNTIME_DIR}"
EOF_ENV
chmod +x "$INSTALL_BIN_DIR/env.sh"

if [[ "$XRIZER_PACKAGE_COMPLIANCE" == "1" ]]; then
  log "packaging GPL license and Corresponding Source"
  ROOT_PROJECT="$ROOT_PROJECT" \
  XRIZER_DIR="$XRIZER_DIR" \
  INSTALL_BIN_DIR="$INSTALL_BIN_DIR" \
  XRIZER_REPO="$XRIZER_REPO" \
  XRIZER_REF_REQUESTED="$XRIZER_REF" \
  XRIZER_VENDOR_CARGO_SOURCES="$XRIZER_VENDOR_CARGO_SOURCES" \
    bash "$ROOT_PROJECT/drivers/xrizer/scripts/linux/package_xrizer_compliance.sh"
else
  log "skip compliance package: XRIZER_PACKAGE_COMPLIANCE=$XRIZER_PACKAGE_COMPLIANCE"
fi

if ! find "$INSTALL_BIN_DIR/runtime" -maxdepth 2 -type f \( -name 'libopenvr_api.so' -o -name 'openvr_api.dll' -o -name 'vrclient.so' \) | grep -q .; then
  log "warning: did not find common OpenVR runtime library names under $INSTALL_BIN_DIR/runtime"
  log "warning: xrizer layout may have changed; inspect target output before using VR_OVERRIDE"
fi

log "installed: $INSTALL_BIN_DIR"
log "runtime:   $INSTALL_BIN_DIR/runtime"
