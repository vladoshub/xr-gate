#!/usr/bin/env bash
# Shared helpers for OpenVR display/profile build and registration scripts.
# Intended to be sourced; does not enable/disable shell options.

openvr_expand_tilde() {
  local value="$1"
  case "$value" in
    "~") printf '%s\n' "$HOME" ;;
    "~/"*) printf '%s\n' "$HOME/${value#"~/"}" ;;
    *) printf '%s\n' "$value" ;;
  esac
}

openvr_normalize_display_frequency_hz() {
  local value="${1:-60}"
  python3 - "$value" <<'PY'
import math
import sys
text = sys.argv[1].strip()
try:
    value = float(text)
except ValueError:
    print(f"[ERROR] Unsupported display frequency: {text}", file=sys.stderr)
    print("Expected finite Hz in range 1..1000.", file=sys.stderr)
    sys.exit(2)
if not math.isfinite(value) or value < 1.0 or value > 1000.0:
    print(f"[ERROR] Unsupported display frequency: {text}", file=sys.stderr)
    print("Expected finite Hz in range 1..1000.", file=sys.stderr)
    sys.exit(2)
# Stable compact representation: 60 -> 60, 59.94 -> 59.94.
print(f"{value:.6f}".rstrip("0").rstrip("."))
PY
}

openvr_frequency_dir_token() {
  local hz="$1"
  hz="${hz//./p}"
  hz="${hz//-/_}"
  printf '%s\n' "$hz"
}

openvr_normalize_display_mode() {
  local value="${1:-direct}"
  value="${value,,}"
  value="${value//-/_}"
  case "$value" in
    direct|direct_mode|drm|drm_lease) printf 'direct\n' ;;
    extended|extended_sbs|sbs|windowed|desktop) printf 'extended_sbs\n' ;;
    *)
      echo "[ERROR] Unsupported OpenVR display mode: $1" >&2
      echo "Expected direct or extended_sbs." >&2
      return 2
      ;;
  esac
}

openvr_normalize_profile_name() {
  local raw="${1:-generic}"
  local value="${raw,,}"
  value="${value//-/_}"
  case "$value" in
    ""|none) value="generic" ;;
    xreal_air2ultra) value="xreal_ultra" ;;
  esac
  if [[ ! "$value" =~ ^[a-z0-9][a-z0-9_.]*$ ]]; then
    echo "[ERROR] Invalid OpenVR device profile: $raw" >&2
    echo "Expected [a-z0-9][a-z0-9_.]* after '-' to '_' normalization." >&2
    return 2
  fi
  printf '%s\n' "$value"
}

openvr_normalize_package_tag() {
  local raw="${1:-}"
  [[ -z "$raw" ]] && { printf '\n'; return 0; }
  local value="${raw,,}"
  value="${value//-/_}"
  value="${value// /_}"
  if [[ ! "$value" =~ ^[a-z0-9][a-z0-9_.]*$ ]]; then
    echo "[ERROR] Invalid XR_OPENVR_PACKAGE_TAG: $raw" >&2
    return 2
  fi
  printf '%s\n' "$value"
}

openvr_driver_dir_name() {
  local hz="$1"
  local mode="$2"
  local profile="${3:-generic}"
  local package_tag="${4:-}"
  local hz_token
  hz_token="$(openvr_frequency_dir_token "$hz")"

  # Preserve historical package names for generic/XREAL builds. Other profiles
  # are namespaced automatically so two devices at the same Hz do not overwrite
  # each other's package. XR_OPENVR_PACKAGE_TAG can further distinguish variants.
  local profile_part=""
  if [[ "$profile" != "generic" && "$profile" != "xreal_ultra" ]]; then
    profile_part="_${profile}"
  fi
  local tag_part=""
  if [[ -n "$package_tag" ]]; then
    tag_part="_${package_tag}"
  fi

  if [[ "$mode" == "direct" ]]; then
    printf 'openvr_driver%s%s_%sHZ\n' "$profile_part" "$tag_part" "$hz_token"
  else
    printf 'openvr_driver%s%s_%sHZ_%s\n' "$profile_part" "$tag_part" "$hz_token" "$mode"
  fi
}

openvr_resolve_device_settings() {
  local driver_root="$1"
  local profile="$2"
  local explicit="${3:-}"

  if [[ -n "$explicit" ]]; then
    explicit="$(openvr_expand_tilde "$explicit")"
    if [[ ! -f "$explicit" ]]; then
      echo "[ERROR] OpenVR device settings overlay not found: $explicit" >&2
      return 2
    fi
    printf '%s\n' "$explicit"
    return 0
  fi

  if [[ "$profile" == "generic" ]]; then
    printf '\n'
    return 0
  fi

  local candidate="$driver_root/devices/$profile/settings/default.vrsettings"
  if [[ ! -f "$candidate" ]]; then
    echo "[ERROR] OpenVR profile '$profile' has no settings overlay:" >&2
    echo "  $candidate" >&2
    echo "Create it, pass XR_OPENVR_DEVICE_SETTINGS=/path/file.vrsettings, or use XR_OPENVR_DEVICE=generic with env overrides." >&2
    return 2
  fi
  printf '%s\n' "$candidate"
}

openvr_resolve_display_config() {
  local driver_root="$1"
  local explicit="${2:-}"

  if [[ -n "$explicit" ]]; then
    explicit="$(openvr_expand_tilde "$explicit")"
    if [[ ! -f "$explicit" ]]; then
      echo "[ERROR] OpenVR display config not found: $explicit" >&2
      return 2
    fi
    printf '%s\n' "$explicit"
    return 0
  fi

  local candidate="$driver_root/configs/display/default.yaml"
  if [[ ! -f "$candidate" ]]; then
    echo "[ERROR] OpenVR default display config not found: $candidate" >&2
    return 2
  fi
  printf '%s\n' "$candidate"
}

openvr_display_config_get() {
  local driver_root="$1"
  local config="$2"
  local dotted_path="$3"
  local parser="$driver_root/scripts/display_optics_config.py"
  if [[ ! -f "$parser" ]]; then
    echo "[ERROR] OpenVR display config parser not found: $parser" >&2
    return 2
  fi
  python3 "$parser" --config "$config" --get "$dotted_path"
}
