#!/usr/bin/env bash
set -euo pipefail

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
    if [[ -d "$d/drivers/xrizer" && -d "$d/devices/xreal_ultra" ]]; then
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
XRIZER_INSTALL_SYSTEM_DEPS="${XRIZER_INSTALL_SYSTEM_DEPS:-1}"
XRIZER_APT_ASSUME_YES="${XRIZER_APT_ASSUME_YES:-1}"
# Packages needed by xrizer build scripts/bindgen on Ubuntu.
XRIZER_SYSTEM_PACKAGES="${XRIZER_SYSTEM_PACKAGES:-build-essential pkg-config clang libclang-dev glslc}"
RUSTUP_INIT_URL="${RUSTUP_INIT_URL:-https://sh.rustup.rs}"

case "$XRIZER_BUILD_PROFILE" in
  release|debug) ;;
  *) fail "unsupported XRIZER_BUILD_PROFILE=$XRIZER_BUILD_PROFILE; expected release or debug" ;;
esac

mkdir -p "$THIRD_PARTY_DIR" "$(dirname "$INSTALL_BIN_DIR")"

if [[ ! -d "$XRIZER_DIR/.git" ]]; then
  [[ "$CLONE_XRIZER" == "1" ]] || fail "xrizer source missing: $XRIZER_DIR and CLONE_XRIZER=$CLONE_XRIZER"
  log "cloning xrizer: $XRIZER_REPO -> $XRIZER_DIR"
  git clone "$XRIZER_REPO" "$XRIZER_DIR"
fi

log "checking out xrizer ref: $XRIZER_REF"
git -C "$XRIZER_DIR" fetch --tags origin || true
git -C "$XRIZER_DIR" checkout "$XRIZER_REF"
git -C "$XRIZER_DIR" submodule update --init --recursive


ensure_system_deps() {
  local missing=()

  command -v gcc >/dev/null 2>&1 || missing+=(build-essential)
  command -v pkg-config >/dev/null 2>&1 || missing+=(pkg-config)
  command -v clang >/dev/null 2>&1 || missing+=(clang)
  command -v glslc >/dev/null 2>&1 || missing+=(glslc)

  if ! ldconfig -p 2>/dev/null | grep -q 'libclang\.so'; then
    missing+=(libclang-dev)
  fi

  if [[ ${#missing[@]} -eq 0 ]]; then
    return 0
  fi

  if [[ "$XRIZER_INSTALL_SYSTEM_DEPS" != "1" ]]; then
    fail "missing system build dependencies: ${missing[*]}. Install manually or rerun with XRIZER_INSTALL_SYSTEM_DEPS=1."
  fi

  command -v apt-get >/dev/null 2>&1 || fail "missing system dependencies (${missing[*]}) and apt-get is unavailable. Install them manually."

  local sudo_cmd=()
  if [[ "${EUID:-$(id -u)}" -ne 0 ]]; then
    command -v sudo >/dev/null 2>&1 || fail "missing system dependencies (${missing[*]}) and sudo is unavailable. Install them manually."
    sudo_cmd=(sudo)
  fi

  local apt_flags=()
  if [[ "$XRIZER_APT_ASSUME_YES" == "1" ]]; then
    apt_flags=(-y)
  fi

  log "installing xrizer system build dependencies: $XRIZER_SYSTEM_PACKAGES"
  "${sudo_cmd[@]}" apt-get update
  # shellcheck disable=SC2086
  "${sudo_cmd[@]}" apt-get install "${apt_flags[@]}" $XRIZER_SYSTEM_PACKAGES

  command -v clang >/dev/null 2>&1 || fail "clang still not found after apt install"
  command -v glslc >/dev/null 2>&1 || fail "glslc still not found after apt install"
}

configure_clang_bindgen_env() {
  if [[ -z "${LIBCLANG_PATH:-}" ]]; then
    local libclang_dir=""
    local candidate
    for candidate in /usr/lib/llvm-*/lib /usr/lib/x86_64-linux-gnu /usr/lib; do
      if compgen -G "$candidate/libclang.so*" >/dev/null; then
        libclang_dir="$candidate"
        break
      fi
    done
    if [[ -n "$libclang_dir" ]]; then
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

  log "cargo-xbuild not found; installing cargo-xbuild"
  cargo install cargo-xbuild --locked

  cargo xbuild --version >/dev/null 2>&1 || fail "cargo xbuild still not available after cargo-xbuild install"
}

ensure_rust_src() {
  if [[ "$XRIZER_INSTALL_RUST_SRC" != "1" ]]; then
    return 0
  fi

  if command -v rustup >/dev/null 2>&1; then
    log "ensuring rust-src component is installed"
    rustup component add rust-src || log "warning: rustup component add rust-src failed; cargo xbuild may fail"
  else
    log "warning: rustup not found; cannot ensure rust-src component"
  fi
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
  (cd "$XRIZER_DIR" && cargo xbuild --release)
  BUILD_OUT="$XRIZER_DIR/target/release"
else
  (cd "$XRIZER_DIR" && cargo xbuild)
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

if ! find "$INSTALL_BIN_DIR/runtime" -maxdepth 2 -type f \( -name 'libopenvr_api.so' -o -name 'openvr_api.dll' -o -name 'vrclient.so' \) | grep -q .; then
  log "warning: did not find common OpenVR runtime library names under $INSTALL_BIN_DIR/runtime"
  log "warning: xrizer layout may have changed; inspect target output before using VR_OVERRIDE"
fi

log "installed: $INSTALL_BIN_DIR"
log "runtime:   $INSTALL_BIN_DIR/runtime"
