#!/usr/bin/env bash
set -euo pipefail

# Command overrides are useful for diagnostics; normal arguments go to xr_client.
case "${1:-}" in
  bash|sh|python3|/bin/bash|/bin/sh)
    exec "$@"
    ;;
esac

export XR_ROOT_PROJECT="${XR_ROOT_PROJECT:-/opt/xr-gate}"
export ROOT_PROJECT="$XR_ROOT_PROJECT"
export AUTO_BUILD=0
export RUNTIME_APP_TCP=1
# Listen on the container interface. Compose publishes ONLY on host 127.0.0.1.
export RUNTIME_APP_TCP_HOST=0.0.0.0
export RUNTIME_APP_TCP_PORT=45810

config_root="${XR_CONFIG_ROOT:-/config}"
config="${XR_PROFILE:-}"
if [[ -f "$config_root/xr_client.json" ]]; then
  config="$config_root/xr_client.json"
elif [[ -z "$config" ]]; then
  echo '[xr-gate] Select XR_PROFILE in your hardware env or provide config/xr_client.json.' >&2
  exit 1
fi
if [[ -f "$config_root/override_controller.json" ]]; then
  # Respect Gear VR's gearvr.json (or the user's explicit CONFIG_PATH).
  python3 - "$config_root" "$config" <<'PY'
import datetime, json, os, shutil, sys
from pathlib import Path
source = Path(sys.argv[1]) / 'override_controller.json'
profile = Path(sys.argv[2])
if not profile.is_file():
    profile = Path(os.environ['XR_ROOT_PROJECT']) / 'bin/python/xr_client/configs' / (sys.argv[2] + '.json')
data = json.loads(profile.read_text(encoding='utf-8-sig')) if profile.is_file() else {}
services = [s for s in data.get('post_gate_services', []) if s.get('name') == 'override_controller']
target = '~/.config/xr_tracking/override_controller/default.json'
if len(services) == 1:
    target = services[0].get('env', {}).get('CONFIG_PATH') or target
destination = Path(os.path.expandvars(os.path.expanduser(target)))
if not destination.is_absolute():
    destination = Path(os.environ['XR_ROOT_PROJECT']) / destination
destination.parent.mkdir(parents=True, exist_ok=True)
if destination.exists() and destination.read_bytes() != source.read_bytes():
    stamp = datetime.datetime.now().strftime('%Y%m%d-%H%M%S-%f')
    shutil.copy2(destination, destination.with_name(f'{destination.stem}.before-{stamp}.json'))
if destination.resolve() != source.resolve():
    shutil.copyfile(source, destination)
PY
fi
if [[ "${XR_VIDEO_TCP:-0}" == 1 ]]; then
  export VIDEO_OUTPUT=both
  export VIDEO_TCP_BIND=0.0.0.0
  export VIDEO_TCP_PORT=45700
fi

python3 "$XR_ROOT_PROJECT/devices/common/linux/scripts/runtime/cpu_target.py" check "$XR_ROOT_PROJECT"
if [[ ! -t 0 ]]; then
  echo '[xr-gate] stdin is not a terminal. Use ./xr-gate run or ./xr-gate console for prompts and keys.' >&2
fi
echo "[xr-gate] release ${XR_VERSION:-unknown}; xr_client config: $config; application TCP: 45810" >&2
exec "$XR_ROOT_PROJECT/run_xr_client.sh" --config "$config" "$@"
