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

normalize_display_frequency_hz() {
  local value="${1:-60}"
  python3 - "$value" <<'PY'
import math
import sys
text = sys.argv[1].strip()
try:
    value = float(text)
except ValueError:
    print(f"[ERROR] Unsupported display frequency: {text}", file=sys.stderr)
    print("Expected integer Hz in range 60..120.", file=sys.stderr)
    sys.exit(2)
if not math.isfinite(value) or abs(value - round(value)) > 1e-6:
    print(f"[ERROR] Unsupported display frequency: {text}", file=sys.stderr)
    print("Expected integer Hz in range 60..120.", file=sys.stderr)
    sys.exit(2)
hz = int(round(value))
if hz < 60 or hz > 120:
    print(f"[ERROR] Unsupported display frequency: {text}", file=sys.stderr)
    print("Expected integer Hz in range 60..120.", file=sys.stderr)
    sys.exit(2)
print(hz)
PY
}

normalize_display_mode() {
  local value="${1:-direct}"
  value="${value,,}"
  value="${value//-/_}"
  case "$value" in
    direct|direct_mode|drm|drm_lease) printf 'direct\n' ;;
    extended|extended_sbs|sbs|windowed|desktop) printf 'extended_sbs\n' ;;
    *)
      echo "[ERROR] Unsupported OpenVR display mode: $1" >&2
      echo "Expected direct or extended_sbs." >&2
      exit 2
      ;;
  esac
}

openvr_driver_dir_name() {
  local hz="$1"
  local mode="$2"
  if [[ "$mode" == "direct" ]]; then
    printf 'openvr_driver_%sHZ\n' "$hz"
  else
    printf 'openvr_driver_%sHZ_%s\n' "$hz" "$mode"
  fi
}

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DRIVER_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

resolve_runtime_root() {
  local explicit="${XR_PACKAGE_ROOT:-${XR_ROOT_PROJECT:-${ROOT_PROJECT:-}}}"
  if [[ -n "$explicit" ]]; then
    explicit="$(expand_tilde "$explicit")"
    if [[ -d "$explicit" ]]; then
      printf '%s\n' "$explicit"
      return 0
    fi
  fi

  # Runtime package layout:
  #   <package>/bin/drivers/openvr_driver_90HZ/scripts/register_driver.sh
  #   <package>/bin/drivers/openvr_driver_60HZ/scripts/register_driver.sh
  # Compatibility with the old unsuffixed package is kept below.
  local package_candidate
  package_candidate="$(cd "$SCRIPT_DIR/../../../.." >/dev/null 2>&1 && pwd || true)"
  if [[ -n "$package_candidate" \
        && -f "$package_candidate/devices/xreal_ultra/xreal_ultra.env" \
        && -d "$package_candidate/bin/drivers" ]]; then
    printf '%s\n' "$package_candidate"
    return 0
  fi

  # Source-tree layout:
  #   <repo>/drivers/openvr_driver/scripts/register_driver.sh
  cd "${DRIVER_ROOT}/../.." && pwd
}

PROJECT_ROOT="$(resolve_runtime_root)"
XR_OPENVR_DISPLAY_FREQUENCY_HZ_RAW="${XR_OPENVR_DISPLAY_FREQUENCY_HZ:-${XR_DISPLAY_FREQUENCY_HZ:-${DISPLAY_FREQUENCY_HZ:-60}}}"
XR_OPENVR_DISPLAY_FREQUENCY_HZ="$(normalize_display_frequency_hz "$XR_OPENVR_DISPLAY_FREQUENCY_HZ_RAW")"
XR_OPENVR_DISPLAY_MODE="$(normalize_display_mode "${XR_OPENVR_DISPLAY_MODE:-${XR_STEAMVR_DISPLAY_MODE:-direct}}")"
XR_OPENVR_DRIVER_DIR_NAME="${XR_OPENVR_DRIVER_DIR_NAME:-$(openvr_driver_dir_name "$XR_OPENVR_DISPLAY_FREQUENCY_HZ" "$XR_OPENVR_DISPLAY_MODE")}"
XR_OPENVR_DEVICE_RAW="${XR_OPENVR_DEVICE:-${XR_DEVICE_TARGET:-${XR_TARGET_DEVICE:-generic}}}"
XR_OPENVR_DEVICE="${XR_OPENVR_DEVICE_RAW,,}"
XR_OPENVR_DEVICE="${XR_OPENVR_DEVICE//-/_}"
case "$XR_OPENVR_DEVICE" in
  generic|none) XR_OPENVR_DEVICE="generic" ;;
  xreal_ultra|xreal_air2ultra) XR_OPENVR_DEVICE="xreal_ultra" ;;
  *)
    echo "[ERROR] Unsupported OpenVR device profile: $XR_OPENVR_DEVICE_RAW" >&2
    echo "Expected generic or xreal_ultra." >&2
    exit 2
    ;;
esac

DRIVERS_ROOT="${DRIVERS_ROOT:-$PROJECT_ROOT/bin/drivers}"
DRIVERS_ROOT="$(expand_tilde "$DRIVERS_ROOT")"
DRIVER_DIR="${DRIVER_PACKAGE:-$DRIVERS_ROOT/$XR_OPENVR_DRIVER_DIR_NAME/xr_tracking}"
DRIVER_DIR="$(expand_tilde "$DRIVER_DIR")"

# Backward compatibility for callers that still pass or expect the old path.
if [[ ! -f "${DRIVER_DIR}/driver.vrdrivermanifest" ]]; then
  LEGACY_DRIVER_DIR="$PROJECT_ROOT/bin/drivers/openvr_driver/xr_tracking"
  if [[ -z "${DRIVER_PACKAGE:-}" && -f "$LEGACY_DRIVER_DIR/driver.vrdrivermanifest" ]]; then
    DRIVER_DIR="$LEGACY_DRIVER_DIR"
  fi
fi

if [[ ! -f "${DRIVER_DIR}/driver.vrdrivermanifest" ]]; then
  echo "ERROR: driver package not found: ${DRIVER_DIR}" >&2
  echo "Build first: devices/xreal_ultra/linux/scripts/install_xreal_ultra_out.sh or drivers/openvr_driver/scripts/build_driver.sh" >&2
  echo "Expected frequency package: $DRIVERS_ROOT/$XR_OPENVR_DRIVER_DIR_NAME/xr_tracking" >&2
  exit 1
fi

# Registration backend:
#   manual    - edit openvrpaths.vrpath directly; does not execute vrpathreg and
#               therefore does not wake Steam/SteamVR on systems where vrpathreg
#               has that side effect.
#   vrpathreg - use Valve's vrpathreg tool.
XR_OPENVR_REGISTER_METHOD="${XR_OPENVR_REGISTER_METHOD:-manual}"
XR_OPENVR_REGISTER_METHOD="${XR_OPENVR_REGISTER_METHOD,,}"
case "$XR_OPENVR_REGISTER_METHOD" in
  manual|direct|json)
    XR_OPENVR_REGISTER_METHOD="manual"
    ;;
  vrpathreg|tool)
    XR_OPENVR_REGISTER_METHOD="vrpathreg"
    ;;
  *)
    echo "ERROR: unsupported XR_OPENVR_REGISTER_METHOD=$XR_OPENVR_REGISTER_METHOD" >&2
    echo "Expected manual or vrpathreg." >&2
    exit 2
    ;;
esac

if [[ "$XR_OPENVR_REGISTER_METHOD" == "vrpathreg" && -z "${VRPATHREG:-}" ]]; then
  CANDIDATES=(
    "$HOME/.local/share/Steam/steamapps/common/SteamVR/bin/linux64/vrpathreg"
    "$HOME/.steam/steam/steamapps/common/SteamVR/bin/linux64/vrpathreg"
    "$HOME/.var/app/com.valvesoftware.Steam/.local/share/Steam/steamapps/common/SteamVR/bin/linux64/vrpathreg"
  )

  for candidate in "${CANDIDATES[@]}"; do
    if [[ -x "$candidate" ]]; then
      VRPATHREG="$candidate"
      break
    fi
  done
fi

if [[ "$XR_OPENVR_REGISTER_METHOD" == "vrpathreg" && ( -z "${VRPATHREG:-}" || ! -x "${VRPATHREG}" ) ]]; then
  echo "ERROR: vrpathreg not found." >&2
  echo "Set explicitly: VRPATHREG=/path/to/vrpathreg ./scripts/register_driver.sh" >&2
  exit 1
fi

if [[ -n "${VRPATHREG:-}" ]]; then
  VRPATHREG_DIR="$(cd "$(dirname "$VRPATHREG")" && pwd)"
else
  VRPATHREG_DIR=""
fi

# In manual registration mode, keep SteamVR/OpenVR as the active runtime by
# default. XRizer is a separate OpenVR-over-OpenXR path and must not remain the
# global OpenVR runtime when registering the native SteamVR driver.
XR_OPENVR_RUNTIME_MODE="${XR_OPENVR_RUNTIME_MODE:-steamvr}"
XR_OPENVR_RUNTIME_MODE="${XR_OPENVR_RUNTIME_MODE,,}"
case "$XR_OPENVR_RUNTIME_MODE" in
  steamvr|steam|openvr)
    XR_OPENVR_RUNTIME_MODE="steamvr"
    ;;
  keep|preserve|unchanged)
    XR_OPENVR_RUNTIME_MODE="keep"
    ;;
  *)
    echo "ERROR: unsupported XR_OPENVR_RUNTIME_MODE=$XR_OPENVR_RUNTIME_MODE" >&2
    echo "Expected steamvr or keep." >&2
    exit 2
    ;;
esac

echo "register:  ${XR_OPENVR_REGISTER_METHOD}"
if [[ "$XR_OPENVR_REGISTER_METHOD" == "vrpathreg" ]]; then
  echo "vrpathreg: ${VRPATHREG}"
fi
echo "driver:    ${DRIVER_DIR}"
echo "frequency: ${XR_OPENVR_DISPLAY_FREQUENCY_HZ}Hz"
echo "mode:      ${XR_OPENVR_DISPLAY_MODE}"
echo "device:    ${XR_OPENVR_DEVICE}"
if [[ "$XR_OPENVR_REGISTER_METHOD" == "manual" ]]; then
  echo "runtime:   ${XR_OPENVR_RUNTIME_MODE}"
fi

patch_steamvr_user_settings() {
  local package_settings="$DRIVER_DIR/resources/settings/default.vrsettings"

  if [[ "${XR_OPENVR_PATCH_STEAMVR_SETTINGS:-1}" != "1" ]]; then
    echo "[register_driver] skip SteamVR user settings patch: XR_OPENVR_PATCH_STEAMVR_SETTINGS=${XR_OPENVR_PATCH_STEAMVR_SETTINGS}" >&2
    return 0
  fi

  if [[ ! -f "$package_settings" ]]; then
    echo "[register_driver][WARN] package settings not found, cannot patch user settings: $package_settings" >&2
    return 0
  fi

  python3 - "$package_settings" "$XR_OPENVR_DISPLAY_FREQUENCY_HZ" "$XR_OPENVR_DISPLAY_MODE" <<'PY'
import json
import sys
from pathlib import Path

package_settings = Path(sys.argv[1])
freq = int(float(sys.argv[2]))
mode = sys.argv[3]

try:
    defaults = json.loads(package_settings.read_text())
except Exception as exc:
    print(f"[register_driver][WARN] failed to read package settings {package_settings}: {exc}", file=sys.stderr)
    sys.exit(0)

xr_defaults = defaults.get("xr_tracking", {})
steamvr_defaults = defaults.get("steamvr", {})

# Only overwrite display/compositor-related settings plus a small set of
# driver-owned compatibility knobs. Keep runtime transport, pose streams, and
# user-specific calibration settings intact.
xr_display_keys = (
    "windowX", "windowY", "windowWidth", "windowHeight",
    "renderWidth", "renderHeight", "displayFrequency",
    "secondsFromVsyncToPhotons", "displayDebugMode",
    "isDisplayOnDesktop", "isDisplayRealDisplay",
)
xr_controller_keys = (
    # Avoid stale per-user settings keeping the visible Vive Wand render model
    # after the package default changed to hidden. Games such as HL Alyx draw
    # their own hands, so the driver model should be hidden unless explicitly
    # requested by this package/config.
    "handControllerRenderModel",
    "handControllerExposeSystemButton",
)
steamvr_display_keys = (
    "directMode", "windowX", "windowY", "windowWidth", "windowHeight",
    "renderWidth", "renderHeight", "displayFrequency",
    "displayDebugMode", "debugMode", "DebugMode",
)

paths = [
    Path.home() / ".config/openvr/config/steamvr.vrsettings",
    Path.home() / ".local/share/Steam/config/steamvr.vrsettings",
    Path.home() / ".steam/steam/config/steamvr.vrsettings",
    Path.home() / ".var/app/com.valvesoftware.Steam/.local/share/Steam/config/steamvr.vrsettings",
]

updated_any = False
for path in paths:
    if not path.exists():
        continue
    try:
        data = json.loads(path.read_text() or "{}")
    except Exception as exc:
        print(f"[register_driver][WARN] skip invalid SteamVR settings {path}: {exc}", file=sys.stderr)
        continue

    # Remove the old project section name. It can leave stale frequency/debug
    # values that are hard to see when grepping logs/configs.
    data.pop("xreal_tracking", None)

    xr = data.setdefault("xr_tracking", {})
    steamvr = data.setdefault("steamvr", {})

    for key in xr_display_keys:
        if key in xr_defaults:
            xr[key] = xr_defaults[key]
    for key in xr_controller_keys:
        if key in xr_defaults:
            xr[key] = xr_defaults[key]
    for key in steamvr_display_keys:
        if key in steamvr_defaults:
            steamvr[key] = steamvr_defaults[key]

    # Keep the per-package controller render-model policy authoritative as well.
    # Older user settings may contain vr_controller_vive_1_5 or an empty string;
    # both can make SteamVR/game render a controller shell over in-game hands.
    if "handControllerRenderModel" in xr_defaults:
        xr["handControllerRenderModel"] = xr_defaults["handControllerRenderModel"]
    if "handControllerExposeSystemButton" in xr_defaults:
        xr["handControllerExposeSystemButton"] = xr_defaults["handControllerExposeSystemButton"]

    # Make the selected package authoritative even if an older package settings
    # file had stale frequency values.
    xr["displayFrequency"] = freq
    steamvr["displayFrequency"] = freq

    if mode == "direct":
        xr["isDisplayOnDesktop"] = False
        xr["isDisplayRealDisplay"] = True
        xr["displayDebugMode"] = False
        steamvr["directMode"] = True
        steamvr["displayDebugMode"] = False
        steamvr["debugMode"] = False
        steamvr["DebugMode"] = False
    elif mode == "extended_sbs":
        xr["isDisplayOnDesktop"] = True
        xr["isDisplayRealDisplay"] = True
        xr["displayDebugMode"] = False
        steamvr["directMode"] = False
        steamvr["displayDebugMode"] = False
        steamvr["debugMode"] = False
        steamvr["DebugMode"] = False

    path.write_text(json.dumps(data, indent=2) + "\n")
    updated_any = True
    print(f"[register_driver] patched SteamVR settings: {path}")

if not updated_any:
    print("[register_driver] no existing SteamVR user settings found to patch")
PY
}

register_driver_manual_openvrpaths() {
  local explicit_openvrpaths="${XR_OPENVRPATHS_FILE:-}"
  local candidates=()

  if [[ -n "$explicit_openvrpaths" ]]; then
    candidates+=("$(expand_tilde "$explicit_openvrpaths")")
  fi

  candidates+=(
    "$HOME/.config/openvr/openvrpaths.vrpath"
    "$HOME/.local/share/Steam/config/openvrpaths.vrpath"
    "$HOME/.steam/steam/config/openvrpaths.vrpath"
    "$HOME/.var/app/com.valvesoftware.Steam/.local/share/Steam/config/openvrpaths.vrpath"
  )

  python3 - "$DRIVER_DIR" "$XR_OPENVR_RUNTIME_MODE" "${XR_OPENVR_STEAMVR_RUNTIME_DIR:-}" "${candidates[@]}" <<'PY'
import json
import sys
from pathlib import Path

if len(sys.argv) < 5:
    print("[register_driver][ERROR] manual registration needs driver, runtime mode, SteamVR runtime override, and openvrpaths candidates", file=sys.stderr)
    sys.exit(2)

driver_dir = str(Path(sys.argv[1]).expanduser().resolve())
runtime_mode = sys.argv[2].strip().lower()
steamvr_runtime_override = sys.argv[3].strip()
raw_candidates = [Path(p).expanduser() for p in sys.argv[4:] if p]

# Deduplicate while preserving order.
candidates = []
seen = set()
for path in raw_candidates:
    key = str(path)
    if key not in seen:
        seen.add(key)
        candidates.append(path)

existing = [p for p in candidates if p.exists()]
targets = existing if existing else candidates[:1]

if not targets:
    print("[register_driver][ERROR] no openvrpaths target", file=sys.stderr)
    sys.exit(2)


def find_steamvr_runtime() -> Path | None:
    raw = []
    if steamvr_runtime_override:
        raw.append(Path(steamvr_runtime_override).expanduser())
    raw.extend([
        Path.home() / ".local/share/Steam/steamapps/common/SteamVR",
        Path.home() / ".steam/steam/steamapps/common/SteamVR",
        Path.home() / ".var/app/com.valvesoftware.Steam/.local/share/Steam/steamapps/common/SteamVR",
    ])
    seen = set()
    for path in raw:
        key = str(path)
        if key in seen:
            continue
        seen.add(key)
        if (path / "bin/linux64/vrserver").exists() or (path / "bin/linux64/vrmonitor").exists():
            return path
        if path.exists() and path.name == "SteamVR":
            return path
    return None


steamvr_runtime = find_steamvr_runtime()
if runtime_mode == "steamvr" and steamvr_runtime is None:
    print("[register_driver][WARN] SteamVR runtime directory was not found; preserving existing runtime entry", file=sys.stderr)


def default_data(path: Path):
    steam_root = Path.home() / ".local/share/Steam"
    data = {
        "jsonid": "vrpathreg",
        "external_drivers": [],
    }
    if steamvr_runtime is not None:
        data["runtime"] = [str(steamvr_runtime)]
    config_dir = steam_root / "config"
    log_dir = steam_root / "logs"
    if config_dir.exists():
        data["config"] = [str(config_dir)]
    if log_dir.exists():
        data["log"] = [str(log_dir)]
    return data


def is_project_xr_tracking_driver(entry: str) -> bool:
    try:
        p = Path(entry).expanduser()
    except Exception:
        return False
    text = str(p)
    # OpenVR external driver package root in this project is always named
    # xr_tracking. Remove stale frequency/mode variants of this same driver, but
    # keep unrelated external drivers intact.
    return p.name == "xr_tracking" and (
        "/openvr_driver" in text
        or "/bin/drivers/" in text
        or "/out/" in text
        or text == driver_dir
    )

updated = []
for path in targets:
    if path.exists():
        try:
            data = json.loads(path.read_text() or "{}")
        except Exception as exc:
            print(f"[register_driver][WARN] invalid {path}, rewriting minimal file: {exc}", file=sys.stderr)
            data = default_data(path)
    else:
        data = default_data(path)

    if not isinstance(data, dict):
        data = default_data(path)

    external = data.get("external_drivers")
    if not isinstance(external, list):
        external = []

    new_external = []
    for entry in external:
        if not isinstance(entry, str) or not entry:
            continue
        if is_project_xr_tracking_driver(entry):
            continue
        if entry not in new_external:
            new_external.append(entry)

    if driver_dir not in new_external:
        new_external.append(driver_dir)

    data["external_drivers"] = new_external
    data.setdefault("jsonid", "vrpathreg")

    if runtime_mode == "steamvr" and steamvr_runtime is not None:
        data["runtime"] = [str(steamvr_runtime)]

    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(data, indent=2) + "\n")
    updated.append(str(path))

print("[register_driver] manual OpenVR registry updated:")
for path in updated:
    print(f"  {path}")
print(f"[register_driver] registered driver: {driver_dir}")
if runtime_mode == "steamvr" and steamvr_runtime is not None:
    print(f"[register_driver] active OpenVR runtime: {steamvr_runtime}")
elif runtime_mode == "keep":
    print("[register_driver] active OpenVR runtime: preserved")
PY
}

if [[ "$XR_OPENVR_REGISTER_METHOD" == "manual" ]]; then
  patch_steamvr_user_settings
  register_driver_manual_openvrpaths
  echo
  echo "[register_driver] manual registration complete."
  echo "[register_driver] To use Valve vrpathreg instead: XR_OPENVR_REGISTER_METHOD=vrpathreg $0"
  exit 0
fi

collect_registered_xr_tracking_packages() {
  LD_LIBRARY_PATH="${VRPATHREG_DIR}:${LD_LIBRARY_PATH:-}" \
    "${VRPATHREG}" show 2>/dev/null | \
    awk -F ':' '
      /^[[:space:]]*xr_tracking[[:space:]]*:/ {
        sub(/^[^:]*:[[:space:]]*/, "", $0)
        gsub(/[[:space:]]+$/, "", $0)
        if ($0 != "") print $0
      }
    '
}

remove_registered_driver_if_present() {
  local package="$1"
  [[ -n "$package" ]] || return 0
  LD_LIBRARY_PATH="${VRPATHREG_DIR}:${LD_LIBRARY_PATH:-}" \
    "${VRPATHREG}" removedriver "$package" >/dev/null 2>&1 || true
}

# Register exactly one OpenVR package. Remove every previously registered
# xr_tracking package reported by vrpathreg plus known source/out/build variants.
# This handles both source-tree builds and out/xreal_ultra packaged builds, and
# avoids the duplicate "xr_tracking : ...openvr_driver_90HZ..." + "xr_tracking :
# ...openvr_driver_60HZ..." state.
shopt -s nullglob
STALE_DRIVER_PACKAGES=(
  "$DRIVER_DIR"
  "$DRIVERS_ROOT/openvr_driver/xr_tracking"
  "$DRIVERS_ROOT"/openvr_driver_*HZ*/xr_tracking
  "$PROJECT_ROOT/build/drivers/openvr_driver/xr_tracking"
  "$PROJECT_ROOT"/build/drivers/openvr_driver_*HZ*/xr_tracking
  "$PROJECT_ROOT/drivers/openvr_driver/build/xr_tracking"
  "$PROJECT_ROOT/bin/drivers/openvr_driver/xr_tracking"
  "$PROJECT_ROOT"/bin/drivers/openvr_driver_*HZ*/xr_tracking
  "$PROJECT_ROOT/out/xreal_ultra/bin/drivers/openvr_driver/xr_tracking"
  "$PROJECT_ROOT"/out/*/bin/drivers/openvr_driver_*HZ*/xr_tracking
  "$HOME/src/xr_tracking/out/xreal_ultra/bin/drivers/openvr_driver/xr_tracking"
  "$HOME/src/xr_tracking"/out/*/bin/drivers/openvr_driver_*HZ*/xr_tracking
  "$HOME/src/xreal_tracking/out/xreal_ultra/bin/drivers/openvr_driver/xr_tracking"
  "$HOME/src/xreal_tracking"/out/*/bin/drivers/openvr_driver_*HZ*/xr_tracking
)
shopt -u nullglob

readarray -t REGISTERED_DRIVER_PACKAGES < <(collect_registered_xr_tracking_packages | awk '!seen[$0]++')

for registered_package in "${REGISTERED_DRIVER_PACKAGES[@]:-}"; do
  echo "[register_driver] removing registered xr_tracking driver: $registered_package"
  remove_registered_driver_if_present "$registered_package"
done

for stale_package in "${STALE_DRIVER_PACKAGES[@]}"; do
  remove_registered_driver_if_present "$stale_package"
done

patch_steamvr_user_settings

LD_LIBRARY_PATH="${VRPATHREG_DIR}:${LD_LIBRARY_PATH:-}" \
  "${VRPATHREG}" adddriver "${DRIVER_DIR}"

echo
echo "[register_driver] active SteamVR OpenVR registry after cleanup:"
LD_LIBRARY_PATH="${VRPATHREG_DIR}:${LD_LIBRARY_PATH:-}" \
  "${VRPATHREG}" show
