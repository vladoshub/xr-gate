#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=./openvr_display_common.sh
source "$SCRIPT_DIR/openvr_display_common.sh"

BUILD_JOBS="${BUILD_JOBS:-$(nproc 2>/dev/null || echo 2)}"

# build_driver.sh is the single-package worker, but accept the plural
# XR_OPENVR_BUILD_FREQUENCIES / XR_OPENVR_BUILD_MODES convenience variables
# when called directly by delegating to the matrix builder. Internal callers set
# XR_OPENVR_SINGLE_VARIANT_BUILD=1 to avoid recursive matrix expansion.
if [[ "${XR_OPENVR_SINGLE_VARIANT_BUILD:-0}" != "1" ]] && { [[ -n "${XR_OPENVR_BUILD_FREQUENCIES:-}" ]] || [[ -n "${XR_OPENVR_BUILD_MODES:-}" ]]; }; then
  echo "[build_driver] Detected XR_OPENVR_BUILD_FREQUENCIES/XR_OPENVR_BUILD_MODES; delegating to build_and_register_driver.sh in build-only mode." >&2
  XR_OPENVR_REGISTER_AFTER_BUILD="${XR_OPENVR_REGISTER_AFTER_BUILD:-0}" \
    exec "${SCRIPT_DIR}/build_and_register_driver.sh"
fi

DRIVER_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
PROJECT_ROOT="$(cd "${DRIVER_ROOT}/../.." && pwd)"

THIRD_PARTY_DIR="${THIRD_PARTY_DIR:-$PROJECT_ROOT/third_party}"
THIRD_PARTY_DIR="$(openvr_expand_tilde "$THIRD_PARTY_DIR")"

OPENVR_REPO_URL="${OPENVR_REPO_URL:-https://github.com/ValveSoftware/openvr.git}"
# Known-good OpenVR SDK commit used by the XR OpenVR driver build.
# Download remains opt-in: set CLONE_OPENVR=1 when the checkout is absent.
OPENVR_DEFAULT_REF="${OPENVR_DEFAULT_REF:-0924064316de3effbcd1acf1e309182a2deb1c05}"
OPENVR_REF="${OPENVR_REF:-$OPENVR_DEFAULT_REF}"
CLONE_OPENVR="${CLONE_OPENVR:-0}"
FETCH_OPENVR="${FETCH_OPENVR:-0}"
ALLOW_DIRTY_OPENVR="${ALLOW_DIRTY_OPENVR:-0}"

OPENVR_THIRD_PARTY_DIR="${OPENVR_THIRD_PARTY_DIR:-$THIRD_PARTY_DIR/openvr}"
OPENVR_THIRD_PARTY_DIR="$(openvr_expand_tilde "$OPENVR_THIRD_PARTY_DIR")"

XR_OPENVR_DEVICE="$(openvr_normalize_profile_name "${XR_OPENVR_DEVICE:-${XR_DEVICE_TARGET:-${XR_TARGET_DEVICE:-generic}}}")"
XR_OPENVR_PACKAGE_TAG="$(openvr_normalize_package_tag "${XR_OPENVR_PACKAGE_TAG:-}")"
XR_OPENVR_DEVICE_SETTINGS="$(openvr_resolve_device_settings "$DRIVER_ROOT" "$XR_OPENVR_DEVICE" "${XR_OPENVR_DEVICE_SETTINGS:-}")"

XR_OPENVR_DISPLAY_FREQUENCY_HZ_RAW="${XR_OPENVR_DISPLAY_FREQUENCY_HZ:-${XR_DISPLAY_FREQUENCY_HZ:-${DISPLAY_FREQUENCY_HZ:-60}}}"
XR_OPENVR_DISPLAY_FREQUENCY_HZ="$(openvr_normalize_display_frequency_hz "$XR_OPENVR_DISPLAY_FREQUENCY_HZ_RAW")"
XR_OPENVR_DISPLAY_MODE="$(openvr_normalize_display_mode "${XR_OPENVR_DISPLAY_MODE:-${XR_STEAMVR_DISPLAY_MODE:-direct}}")"
XR_OPENVR_DRIVER_DIR_NAME="${XR_OPENVR_DRIVER_DIR_NAME:-$(openvr_driver_dir_name "$XR_OPENVR_DISPLAY_FREQUENCY_HZ" "$XR_OPENVR_DISPLAY_MODE" "$XR_OPENVR_DEVICE" "$XR_OPENVR_PACKAGE_TAG")}"

BUILD_DIR="${BUILD_DIR:-$PROJECT_ROOT/build/drivers/${XR_OPENVR_DRIVER_DIR_NAME}}"
BUILD_DIR="$(openvr_expand_tilde "$BUILD_DIR")"

INSTALL_DRIVERS_ROOT="${INSTALL_DRIVERS_ROOT:-$PROJECT_ROOT/bin/drivers}"
INSTALL_DRIVERS_ROOT="$(openvr_expand_tilde "$INSTALL_DRIVERS_ROOT")"
INSTALL_DRIVER_ROOT="${INSTALL_DRIVER_ROOT:-$INSTALL_DRIVERS_ROOT/$XR_OPENVR_DRIVER_DIR_NAME}"
INSTALL_DRIVER_ROOT="$(openvr_expand_tilde "$INSTALL_DRIVER_ROOT")"

# Backward compatibility: older callers passed .../bin/drivers/openvr_driver as
# INSTALL_DRIVER_ROOT. Convert that generic root into the frequency-specific
# package root unless explicitly disabled.
if [[ "${XR_OPENVR_KEEP_UNSUFFIXED_DRIVER_ROOT:-0}" != "1" && "$(basename "$INSTALL_DRIVER_ROOT")" == "openvr_driver" ]]; then
  INSTALL_DRIVER_ROOT="$(dirname "$INSTALL_DRIVER_ROOT")/$XR_OPENVR_DRIVER_DIR_NAME"
fi

INSTALL_DRIVER_PACKAGE="${INSTALL_DRIVER_PACKAGE:-$INSTALL_DRIVER_ROOT/xr_tracking}"
INSTALL_DRIVER_PACKAGE="$(openvr_expand_tilde "$INSTALL_DRIVER_PACKAGE")"
if [[ "${XR_OPENVR_KEEP_UNSUFFIXED_DRIVER_ROOT:-0}" != "1" && "$(basename "$(dirname "$INSTALL_DRIVER_PACKAGE")")" == "openvr_driver" ]]; then
  INSTALL_DRIVER_PACKAGE="$(dirname "$(dirname "$INSTALL_DRIVER_PACKAGE")")/${XR_OPENVR_DRIVER_DIR_NAME}/$(basename "$INSTALL_DRIVER_PACKAGE")"
fi

is_git_commit_sha() {
  local ref="$1"
  [[ "$ref" =~ ^[0-9a-fA-F]{40}$ ]]
}

clone_openvr_checkout() {
  local url="$1"
  local dest="$2"
  local ref="$3"

  if [[ -z "$ref" ]]; then
    git clone --depth 1 "$url" "$dest" >&2
    return 0
  fi

  if is_git_commit_sha "$ref"; then
    mkdir -p "$dest"
    git -C "$dest" init >&2
    git -C "$dest" remote add origin "$url" >&2

    # GitHub supports fetching reachable commits by SHA in normal cases. Keep a
    # full-fetch fallback for older git/server combinations where direct SHA
    # fetch is disabled.
    if ! git -C "$dest" fetch --depth 1 origin "$ref" >&2; then
      echo "[build_driver][WARN] Shallow fetch by OpenVR commit failed; falling back to full fetch." >&2
      git -C "$dest" fetch --tags origin >&2
    fi

    git -C "$dest" checkout --quiet --detach "$ref" >&2
    return 0
  fi

  git clone --branch "$ref" --depth 1 "$url" "$dest" >&2
}

fetch_openvr_ref_if_needed() {
  local repo="$1"
  local ref="$2"

  git -C "$repo" fetch --tags --prune origin >&2

  if [[ -n "$ref" ]] && is_git_commit_sha "$ref"; then
    if ! git -C "$repo" rev-parse --verify --quiet "$ref^{commit}" >/dev/null; then
      git -C "$repo" fetch --depth 1 origin "$ref" >&2 || true
    fi
  fi
}

resolve_openvr_sdk_root() {
  local explicit="${XR_OPENVR_SDK_ROOT:-${OPENVR_SDK_ROOT:-}}"
  if [[ -n "$explicit" ]]; then
    explicit="$(openvr_expand_tilde "$explicit")"
    if [[ -f "$explicit/headers/openvr_driver.h" ]]; then
      printf '%s\n' "$explicit"
      return 0
    fi
    echo "[ERROR] XR_OPENVR_SDK_ROOT/OPENVR_SDK_ROOT is set but OpenVR header was not found:" >&2
    echo "  $explicit/headers/openvr_driver.h" >&2
    exit 2
  fi

  if [[ -f "$OPENVR_THIRD_PARTY_DIR/headers/openvr_driver.h" ]]; then
    printf '%s\n' "$OPENVR_THIRD_PARTY_DIR"
    return 0
  fi

  if [[ "$CLONE_OPENVR" == "1" ]]; then
    mkdir -p "$(dirname "$OPENVR_THIRD_PARTY_DIR")"
    if [[ -e "$OPENVR_THIRD_PARTY_DIR" && ! -d "$OPENVR_THIRD_PARTY_DIR/.git" ]]; then
      echo "[ERROR] OPENVR_THIRD_PARTY_DIR exists but is not a git checkout:" >&2
      echo "  $OPENVR_THIRD_PARTY_DIR" >&2
      exit 2
    fi

    if [[ ! -d "$OPENVR_THIRD_PARTY_DIR/.git" ]]; then
      echo "[build_driver] Cloning OpenVR SDK:" >&2
      echo "  url:  $OPENVR_REPO_URL" >&2
      echo "  ref:  ${OPENVR_REF:-<default-branch>}" >&2
      echo "  dest: $OPENVR_THIRD_PARTY_DIR" >&2
      clone_openvr_checkout "$OPENVR_REPO_URL" "$OPENVR_THIRD_PARTY_DIR" "$OPENVR_REF"
    fi
  fi

  if [[ ! -d "$OPENVR_THIRD_PARTY_DIR/.git" ]]; then
    echo "[ERROR] OpenVR SDK not found." >&2
    echo "[ERROR] Expected default third_party checkout:" >&2
    echo "  $OPENVR_THIRD_PARTY_DIR" >&2
    echo "" >&2
    echo "Either set XR_OPENVR_SDK_ROOT/OPENVR_SDK_ROOT, or clone into third_party:" >&2
    echo "  CLONE_OPENVR=1 $SCRIPT_DIR/build_driver.sh" >&2
    echo "" >&2
    echo "Optional variables:" >&2
    echo "  OPENVR_REPO_URL=$OPENVR_REPO_URL" >&2
    echo "  OPENVR_REF=<branch-tag-or-commit>  # default: $OPENVR_REF" >&2
    echo "  OPENVR_THIRD_PARTY_DIR=$OPENVR_THIRD_PARTY_DIR" >&2
    exit 2
  fi

  if [[ "$FETCH_OPENVR" == "1" ]]; then
    fetch_openvr_ref_if_needed "$OPENVR_THIRD_PARTY_DIR" "$OPENVR_REF"
  fi

  if [[ "$ALLOW_DIRTY_OPENVR" != "1" ]]; then
    if ! git -C "$OPENVR_THIRD_PARTY_DIR" diff --quiet || ! git -C "$OPENVR_THIRD_PARTY_DIR" diff --cached --quiet; then
      echo "[ERROR] OpenVR SDK checkout has uncommitted changes:" >&2
      echo "  $OPENVR_THIRD_PARTY_DIR" >&2
      echo "Set ALLOW_DIRTY_OPENVR=1 to build anyway." >&2
      exit 2
    fi
  fi

  if [[ -n "$OPENVR_REF" ]]; then
    if git -C "$OPENVR_THIRD_PARTY_DIR" rev-parse --verify --quiet "$OPENVR_REF^{commit}" >/dev/null; then
      git -C "$OPENVR_THIRD_PARTY_DIR" checkout --quiet "$OPENVR_REF" >&2
    elif git -C "$OPENVR_THIRD_PARTY_DIR" rev-parse --verify --quiet "origin/$OPENVR_REF^{commit}" >/dev/null; then
      git -C "$OPENVR_THIRD_PARTY_DIR" checkout --quiet "origin/$OPENVR_REF" >&2
    else
      echo "[ERROR] OPENVR_REF not found in checkout: $OPENVR_REF" >&2
      echo "Try FETCH_OPENVR=1 or use an existing branch/tag/commit." >&2
      echo "Default OpenVR commit: $OPENVR_DEFAULT_REF" >&2
      exit 2
    fi
  fi

  if [[ ! -f "$OPENVR_THIRD_PARTY_DIR/headers/openvr_driver.h" ]]; then
    echo "[ERROR] OpenVR header not found after resolving SDK:" >&2
    echo "  $OPENVR_THIRD_PARTY_DIR/headers/openvr_driver.h" >&2
    exit 2
  fi

  printf '%s\n' "$OPENVR_THIRD_PARTY_DIR"
}

# resolve_openvr_sdk_root is used in command substitution, so every
# non-return-value command inside it must write diagnostics to stderr only.
SDK_ROOT="$(resolve_openvr_sdk_root)"

printf '[build_driver] PROJECT_ROOT=%s\n' "$PROJECT_ROOT"
printf '[build_driver] DRIVER_ROOT=%s\n' "$DRIVER_ROOT"
printf '[build_driver] OPENVR_SDK_ROOT=%s\n' "$SDK_ROOT"
printf '[build_driver] OPENVR_REF=%s\n' "${OPENVR_REF:-<default-branch>}"
printf '[build_driver] BUILD_DIR=%s\n' "$BUILD_DIR"
printf '[build_driver] DRIVER_DIR_NAME=%s\n' "$XR_OPENVR_DRIVER_DIR_NAME"
printf '[build_driver] INSTALL_DRIVER_ROOT=%s\n' "$INSTALL_DRIVER_ROOT"
printf '[build_driver] INSTALL_DRIVER_PACKAGE=%s\n' "$INSTALL_DRIVER_PACKAGE"
printf '[build_driver] DISPLAY_FREQUENCY_HZ=%s\n' "$XR_OPENVR_DISPLAY_FREQUENCY_HZ"
printf '[build_driver] DISPLAY_MODE=%s\n' "$XR_OPENVR_DISPLAY_MODE"
printf '[build_driver] DEVICE=%s\n' "$XR_OPENVR_DEVICE"
printf '[build_driver] DEVICE_SETTINGS=%s\n' "${XR_OPENVR_DEVICE_SETTINGS:-<none>}"
printf '[build_driver] PACKAGE_TAG=%s\n' "${XR_OPENVR_PACKAGE_TAG:-<none>}"

cmake -S "$DRIVER_ROOT" -B "$BUILD_DIR" -DXR_OPENVR_SDK_ROOT="$SDK_ROOT"
cmake --build "$BUILD_DIR" -j"$BUILD_JOBS"

PACKAGE_SETTINGS="$BUILD_DIR/xr_tracking/resources/settings/default.vrsettings"
if [[ ! -f "$PACKAGE_SETTINGS" ]]; then
  echo "[ERROR] Built driver package settings file not found:" >&2
  echo "  $PACKAGE_SETTINGS" >&2
  exit 2
fi

render_settings_args=(
  --settings "$PACKAGE_SETTINGS"
  --device-profile "$XR_OPENVR_DEVICE"
  --display-frequency "$XR_OPENVR_DISPLAY_FREQUENCY_HZ"
  --display-mode "$XR_OPENVR_DISPLAY_MODE"
)
if [[ -n "$XR_OPENVR_DEVICE_SETTINGS" ]]; then
  render_settings_args+=(--device-settings "$XR_OPENVR_DEVICE_SETTINGS")
fi
"$SCRIPT_DIR/render_display_settings.py" "${render_settings_args[@]}"

echo "[build_driver] Patched package device=$XR_OPENVR_DEVICE displayFrequency=$XR_OPENVR_DISPLAY_FREQUENCY_HZ displayMode=$XR_OPENVR_DISPLAY_MODE in $PACKAGE_SETTINGS"

BUILD_DRIVER_PACKAGE="$BUILD_DIR/xr_tracking"
BUILD_DRIVER_SO="$BUILD_DRIVER_PACKAGE/bin/linux64/driver_xr_tracking.so"
if [[ ! -f "$BUILD_DRIVER_SO" ]]; then
  echo "[ERROR] Built driver shared library not found:" >&2
  echo "  $BUILD_DRIVER_SO" >&2
  exit 2
fi

# SteamVR must be able to run the driver package without the build tree. Copy the
# complete assembled package into bin/ and register that stable package path.
# This also keeps the OpenVR runtime library next to driver_xr_tracking.so so the
# package survives deleting $PROJECT_ROOT/build.
mkdir -p "$INSTALL_DRIVER_ROOT"
rm -rf "$INSTALL_DRIVER_PACKAGE.tmp" "$INSTALL_DRIVER_PACKAGE"
cp -a "$BUILD_DRIVER_PACKAGE" "$INSTALL_DRIVER_PACKAGE.tmp"
mv "$INSTALL_DRIVER_PACKAGE.tmp" "$INSTALL_DRIVER_PACKAGE"

echo "[build_driver] Installed driver package: $INSTALL_DRIVER_PACKAGE"
echo "Driver package: $INSTALL_DRIVER_PACKAGE"
