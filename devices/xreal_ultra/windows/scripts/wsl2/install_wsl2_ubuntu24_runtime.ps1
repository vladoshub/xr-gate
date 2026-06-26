param(
  [string]$Root = "",
  [string]$Distro = "",
  [string]$RuntimeSourceRoot = "",
  [string]$WslProjectRoot = "",
  [string]$WslOutName = "",
  [string]$WslRuntimeRoot = "",
  [string]$NetworkConfig = "",
  [switch]$SkipRuntimeDeps,
  [switch]$NoCopyRuntime
)

$ErrorActionPreference = "Stop"
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
if ([string]::IsNullOrWhiteSpace($Root)) {
  $Root = (Resolve-Path (Join-Path $ScriptDir "..\..\..\..\..")).Path
} else {
  $Root = (Resolve-Path $Root).Path
}
$env:ROOT_PROJECT = $Root
$env:XR_ROOT_PROJECT = $Root
$ScriptsRoot = (Resolve-Path (Join-Path $ScriptDir "..")).Path
. (Join-Path $ScriptsRoot "xreal_ultra_out_env.ps1")
. (Join-Path $ScriptsRoot "load_windows_wsl2_network_env.ps1")
$ResolvedNetworkConfig = Import-XrWindowsNetworkEnv -Root $Root -NetworkConfig $NetworkConfig

if ([string]::IsNullOrWhiteSpace($Distro)) {
  $Distro = Get-XrEnvOrDefault "XR_WSL2_DISTRO" "Ubuntu-24.04"
}
if ([string]::IsNullOrWhiteSpace($WslProjectRoot)) {
  $WslProjectRoot = Get-XrEnvOrDefault "XR_WSL_PROJECT_ROOT" "~/src/xr_tracking"
}
if ([string]::IsNullOrWhiteSpace($RuntimeSourceRoot)) {
  # This is expected to be a Linux runtime package copied to Windows, e.g.
  # C:\src\xr_tracking\out\xreal_ultra.  Its contents are mirrored into
  # WSL2 under ~/src/xr_tracking/out/<leaf-name>.
  $RuntimeSourceRoot = Join-Path $Root "out\xreal_ultra"
}
$RuntimeSourceRoot = (Resolve-Path $RuntimeSourceRoot).Path
if ([string]::IsNullOrWhiteSpace($WslOutName)) {
  $WslOutName = Get-XrEnvOrDefault "XR_WSL_OUT_NAME" (Split-Path -Leaf $RuntimeSourceRoot)
}
if ([string]::IsNullOrWhiteSpace($WslRuntimeRoot)) {
  $WslRuntimeRoot = "$WslProjectRoot/out/$WslOutName"
}

function Invoke-Wsl([string]$Command) {
  Write-Host "[wsl2_runtime] $Command"
  wsl.exe -d $Distro -- bash -lc $Command
  if ($LASTEXITCODE -ne 0) { throw "WSL command failed with code $LASTEXITCODE" }
}

function Quote-Bash([string]$Value) {
  return "'" + $Value.Replace("'", "'\''") + "'"
}

$EscapedRoot = $Root.Replace("'", "'\''")
$LinuxRoot = (& wsl.exe -d $Distro -- bash -lc "wslpath -a '$EscapedRoot'").Trim()
if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace($LinuxRoot)) {
  throw "failed to resolve Windows root inside WSL2: $Root"
}

$EscapedRuntimeSourceRoot = $RuntimeSourceRoot.Replace("'", "'\''")
$LinuxRuntimeSourceRoot = (& wsl.exe -d $Distro -- bash -lc "wslpath -a '$EscapedRuntimeSourceRoot'").Trim()
if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace($LinuxRuntimeSourceRoot)) {
  throw "failed to resolve runtime source inside WSL2: $RuntimeSourceRoot"
}

$EscapedRuntimeRoot = $WslRuntimeRoot.Replace("'", "'\''")
$RuntimeRootResolved = (& wsl.exe -d $Distro -- bash -lc "mkdir -p '$EscapedRuntimeRoot' && cd '$EscapedRuntimeRoot' && pwd").Trim()
if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace($RuntimeRootResolved)) {
  throw "failed to create WSL runtime root: $WslRuntimeRoot"
}

if (-not $SkipRuntimeDeps) {
  $DepsScript = "$LinuxRoot/devices/xreal_ultra/linux/scripts/install_runtime_deps_ubuntu24.sh"
  Invoke-Wsl "test -x $(Quote-Bash $DepsScript) || chmod +x $(Quote-Bash $DepsScript)"
  Invoke-Wsl "$(Quote-Bash $DepsScript) --no-groups --no-udev --no-venv"
}

if (-not $NoCopyRuntime) {
  if (-not (Test-Path $RuntimeSourceRoot)) {
    throw "RuntimeSourceRoot does not exist: $RuntimeSourceRoot"
  }
  Invoke-Wsl "mkdir -p $(Quote-Bash $RuntimeRootResolved)"
  Invoke-Wsl "rsync -a --delete $(Quote-Bash ($LinuxRuntimeSourceRoot + '/')) $(Quote-Bash ($RuntimeRootResolved + '/'))"
}

Invoke-Wsl "mkdir -p $(Quote-Bash "$RuntimeRootResolved/bin/backends/basalt_vio") $(Quote-Bash "$RuntimeRootResolved/bin/backends/imu_3dof") $(Quote-Bash "$RuntimeRootResolved/bin/backends/mercury_hand_tracking") $(Quote-Bash "$RuntimeRootResolved/bin/backends/xr_video") $(Quote-Bash "$RuntimeRootResolved/bin/backends/xr_spatial") $(Quote-Bash "$RuntimeRootResolved/bin/bridges") $(Quote-Bash "$RuntimeRootResolved/bin/runtime_adapters/xr_runtime_adapter") $(Quote-Bash "$RuntimeRootResolved/configs") $(Quote-Bash "$RuntimeRootResolved/logs") $(Quote-Bash "$RuntimeRootResolved/run") $(Quote-Bash "$RuntimeRootResolved/scripts")"

# Copy the Windows/WSL network profile into the WSL runtime package.
if ($ResolvedNetworkConfig -and (Test-Path $ResolvedNetworkConfig)) {
  $EscapedConfig = $ResolvedNetworkConfig.Replace("'", "'\''")
  $LinuxConfig = (& wsl.exe -d $Distro -- bash -lc "wslpath -a '$EscapedConfig'").Trim()
  Invoke-Wsl "cp $(Quote-Bash $LinuxConfig) $(Quote-Bash "$RuntimeRootResolved/configs/windows_wsl2_network.env")"
}

$StartScript = @'
#!/usr/bin/env bash
set -euo pipefail

RUNTIME_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CONFIG="${XR_WINDOWS_NETWORK_CONFIG:-$RUNTIME_ROOT/configs/windows_wsl2_network.env}"
LOG_DIR="${XR_WSL_LOG_DIR:-$RUNTIME_ROOT/logs}"
RUN_DIR="${XR_WSL_RUN_DIR:-$RUNTIME_ROOT/run}"
mkdir -p "$LOG_DIR" "$RUN_DIR"

load_env_file() {
  local path="$1"
  [[ -f "$path" ]] || return 0
  while IFS= read -r line || [[ -n "$line" ]]; do
    line="${line%$'\r'}"
    [[ -z "$line" || "${line:0:1}" == "#" ]] && continue
    [[ "$line" == *=* ]] || continue
    local key="${line%%=*}"
    local value="${line#*=}"
    key="$(printf '%s' "$key" | tr -d '[:space:]')"
    value="${value#\"}"; value="${value%\"}"
    value="${value#\'}"; value="${value%\'}"
    if [[ -n "$key" && -z "${!key+x}" ]]; then
      export "$key=$value"
    fi
  done < "$path"
}

resolve_windows_host() {
  local configured="${1:-auto}"
  if [[ -n "$configured" && "$configured" != "auto" ]]; then
    printf '%s\n' "$configured"
    return 0
  fi
  awk '/^nameserver[[:space:]]+/ {print $2; exit}' /etc/resolv.conf 2>/dev/null || true
}

load_env_file "$CONFIG"

WINDOWS_HOST="$(resolve_windows_host "${XR_WINDOWS_HOST_FROM_WSL:-auto}")"
[[ -n "$WINDOWS_HOST" ]] || WINDOWS_HOST="127.0.0.1"
CAPTURE_HOST="$(resolve_windows_host "${XR_CAPTURE_TCP_HOST_FROM_WSL:-$WINDOWS_HOST}")"
OVERRIDE_HOST="$(resolve_windows_host "${XR_OVERRIDE_CONTROLLER_TCP_HOST_FROM_WSL:-$WINDOWS_HOST}")"
RUNTIME_UDP_HOST="$(resolve_windows_host "${XR_RUNTIME_OUTPUT_UDP_HOST_FROM_WSL:-$WINDOWS_HOST}")"
TRACKING_UDP_HOST="$(resolve_windows_host "${XR_TRACKING_UDP_TARGET_HOST_FROM_WSL:-$WINDOWS_HOST}")"

CAPTURE_PORT="${XR_CAPTURE_TCP_PORT:-45660}"
OVERRIDE_PORT="${XR_OVERRIDE_CONTROLLER_TCP_PORT:-45672}"
RUNTIME_POSE_UDP_PORT="${XR_RUNTIME_POSE_UDP_PORT:-45800}"
RUNTIME_HAND_UDP_PORT="${XR_RUNTIME_HAND_UDP_PORT:-45801}"
RUNTIME_CONTROLLER_STATE_UDP_PORT="${XR_RUNTIME_CONTROLLER_STATE_UDP_PORT:-45802}"
TRACKING_UDP_TARGET_PORT="${XR_TRACKING_UDP_TARGET_PORT:-45670}"
SPATIAL_PROXY_MESH_UDP_PORT="${XR_SPATIAL_PROXY_MESH_UDP_PORT:-45740}"

export ROOT_PROJECT="$RUNTIME_ROOT"
export XR_ROOT_PROJECT="$RUNTIME_ROOT"
export XR_BIN_ROOT="$RUNTIME_ROOT/bin"
export LD_LIBRARY_PATH="$RUNTIME_ROOT/bin/backends/basalt_vio/lib:$RUNTIME_ROOT/bin/backends/mercury_hand_tracking:$RUNTIME_ROOT/bin/runtime_adapters/xr_runtime_adapter:${LD_LIBRARY_PATH:-}"
export TRACKING_REGISTRY="${XR_WSL_TRACKING_REGISTRY:-/tmp/tracking_streams.json}"
export RUNTIME_TRACKING_REGISTRY="${XR_WSL_RUNTIME_TRACKING_REGISTRY:-/tmp/runtime_tracking_streams.json}"

pids=()

start_bg() {
  local name="$1"; shift
  local log="$LOG_DIR/$name.log"
  echo "[wsl2_runtime] start $name -> $log"
  ("$@") >"$log" 2>&1 &
  local pid=$!
  echo "$pid" > "$RUN_DIR/$name.pid"
  pids+=("$pid")
}

start_optional() {
  local name="$1"; shift
  local exe="$1"
  if [[ ! -x "$exe" ]]; then
    echo "[wsl2_runtime][WARN] skip $name: missing executable $exe" >&2
    return 0
  fi
  start_bg "$name" "$@"
}

stop_all() {
  echo "[wsl2_runtime] stopping ${#pids[@]} processes"
  for pid in "${pids[@]}"; do
    kill -TERM "$pid" 2>/dev/null || true
  done
  sleep 1
  for pid in "${pids[@]}"; do
    kill -KILL "$pid" 2>/dev/null || true
  done
}
trap stop_all INT TERM EXIT

FINAL_PROFILE_DIR="${FINAL_PROFILE_DIR:-$RUNTIME_ROOT/calibration_dataset/final/${XR_DEVICE_NAME:-xreal_air2ultra}/${XR_SERIAL:-ZBBM5DZFMP}/${CALIB_PROFILE_NAME:-unified_480_ccw90}}"
BASALT_BIN="$RUNTIME_ROOT/bin/backends/basalt_vio/capture_basalt_backend"
HAND_BIN="$RUNTIME_ROOT/bin/backends/mercury_hand_tracking/capture_hand_tracking_backend"
IMU3DOF_BIN="$RUNTIME_ROOT/bin/backends/imu_3dof/imu_3dof_backend"
XR_RUNTIME_ADAPTER_BIN="$RUNTIME_ROOT/bin/runtime_adapters/xr_runtime_adapter/xr_runtime_adapter"
TRACKING_UDP_BRIDGE_BIN="$RUNTIME_ROOT/bin/bridges/tracking_udp_bridge"

if [[ "${RUN_WSL_BASALT_VIO:-1}" == "1" ]]; then
  start_optional basalt_vio "$BASALT_BIN" "$BASALT_BIN" \
    --transport capture_tcp \
    --tcp-host "$CAPTURE_HOST" \
    --tcp-port "$CAPTURE_PORT" \
    --cam0-stream camera0 \
    --cam1-stream camera1 \
    --imu-stream imu0 \
    --cam-calib "$FINAL_PROFILE_DIR/basalt_calib_unified_480_ccw90.json" \
    --config-path "$FINAL_PROFILE_DIR/basalt_vio_config_unified_480_ccw90.json" \
    --out-dir "${XR_WSL_BASALT_OUT_DIR:-/tmp/xr_basalt_windows_wsl2_live}" \
    --duration 0 \
    --image-scale 256 \
    --no-enforce-realtime
fi

if [[ "${RUN_WSL_IMU_3DOF:-0}" == "1" ]]; then
  start_optional imu_3dof "$IMU3DOF_BIN" "$IMU3DOF_BIN" \
    --transport capture_tcp \
    --tcp-host "$CAPTURE_HOST" \
    --tcp-port "$CAPTURE_PORT" \
    --imu-stream imu0
fi

if [[ "${RUN_WSL_HAND_TRACKING:-1}" == "1" ]]; then
  MERCURY_DIR="$RUNTIME_ROOT/bin/backends/mercury_hand_tracking"
  MERCURY_MODELS="${MERCURY_MODELS:-$RUNTIME_ROOT/bin/hand-tracking-models/mercury}"
  MERCURY_CALIB="${MERCURY_CALIB:-$FINAL_PROFILE_DIR/mercury_calib_unified_480_ccw90.json}"
  MERCURY_LIB="${MERCURY_LIB:-$MERCURY_DIR/libxr_mercury_runtime.so}"
  start_optional hand_tracking "$HAND_BIN" "$HAND_BIN" \
    --transport capture_tcp \
    --tcp-host "$CAPTURE_HOST" \
    --tcp-port "$CAPTURE_PORT" \
    --cam0-stream camera0 \
    --cam1-stream camera1 \
    --imu-stream imu0 \
    --duration 0 \
    --hand-format-version 2 \
    --hand-tracker mercury \
    --mercury-runtime-lib "$MERCURY_LIB" \
    --mercury-models "$MERCURY_MODELS" \
    --mercury-calib "$MERCURY_CALIB" \
    --mercury-min-detection-confidence "${MERCURY_MIN_DETECTION_CONFIDENCE:-0.10}" \
    --print-every "${HAND_PRINT_EVERY:-30}"
fi

if [[ "${RUN_WSL_XR_RUNTIME_ADAPTER:-1}" == "1" ]]; then
  start_optional xr_runtime_adapter "$XR_RUNTIME_ADAPTER_BIN" "$XR_RUNTIME_ADAPTER_BIN" \
    --adapter logging \
    --mode tick \
    --input shm \
    --registry "$TRACKING_REGISTRY" \
    --tracking-transform-config "${TRACKING_TRANSFORM_CONFIG:-$RUNTIME_ROOT/runtime_adapters/xr_runtime_adapter/configs/xr_21_joint_hand_viewer_verified.json}" \
    --hmd-stream hmd_pose \
    --hand-stream hand_tracking \
    --controller-input-transport tcp \
    --controller-input-host "$OVERRIDE_HOST" \
    --controller-input-port "$OVERRIDE_PORT" \
    --publish-runtime-pose-udp \
    --runtime-pose-udp-host "$RUNTIME_UDP_HOST" \
    --runtime-pose-udp-port "$RUNTIME_POSE_UDP_PORT" \
    --publish-runtime-hand-udp \
    --runtime-hand-udp-host "$RUNTIME_UDP_HOST" \
    --runtime-hand-udp-port "$RUNTIME_HAND_UDP_PORT" \
    --publish-runtime-controller-state-udp \
    --runtime-controller-state-udp-host "$RUNTIME_UDP_HOST" \
    --runtime-controller-state-udp-port "$RUNTIME_CONTROLLER_STATE_UDP_PORT" \
    --tick-rate "${XR_RUNTIME_TICK_RATE:-90}" \
    --prediction-ms "${XR_RUNTIME_PREDICTION_MS:-15}" \
    --print-every "${XR_RUNTIME_PRINT_EVERY:-90}" \
    --duration 0
fi

if [[ "${RUN_WSL_TRACKING_UDP_BRIDGE:-0}" == "1" ]]; then
  start_optional tracking_udp_bridge "$TRACKING_UDP_BRIDGE_BIN" "$TRACKING_UDP_BRIDGE_BIN" \
    --registry "$TRACKING_REGISTRY" \
    --hmd-stream hmd_pose \
    --hand-stream hand_tracking \
    --target-host "$TRACKING_UDP_HOST" \
    --target-port "$TRACKING_UDP_TARGET_PORT" \
    --mode "${TRACKING_UDP_MODE:-event}" \
    --spatial-proxy-mesh-input "${SPATIAL_PROXY_MESH_INPUT:-shm}" \
    --spatial-proxy-mesh-registry "$RUNTIME_TRACKING_REGISTRY" \
    --spatial-proxy-mesh-stream "${SPATIAL_PROXY_MESH_STREAM:-spatial_proxy_mesh}" \
    --spatial-proxy-mesh-udp-host "$TRACKING_UDP_HOST" \
    --spatial-proxy-mesh-udp-port "$SPATIAL_PROXY_MESH_UDP_PORT"
fi

echo "[wsl2_runtime] runtime root: $RUNTIME_ROOT"
echo "[wsl2_runtime] Windows host from WSL: $WINDOWS_HOST"
echo "[wsl2_runtime] capture tcp: $CAPTURE_HOST:$CAPTURE_PORT"
echo "[wsl2_runtime] runtime UDP target: $RUNTIME_UDP_HOST pose=$RUNTIME_POSE_UDP_PORT hand=$RUNTIME_HAND_UDP_PORT controller=$RUNTIME_CONTROLLER_STATE_UDP_PORT"
echo "[wsl2_runtime] logs: $LOG_DIR"

while true; do
  sleep 1
  alive=0
  for pid in "${pids[@]}"; do
    if kill -0 "$pid" 2>/dev/null; then
      alive=$((alive + 1))
    fi
  done
  if [[ "$alive" -eq 0 && "${#pids[@]}" -gt 0 ]]; then
    echo "[wsl2_runtime][ERROR] all child processes exited" >&2
    exit 1
  fi
done
'@
$TempScript = New-TemporaryFile
Set-Content -Encoding UTF8 -Path $TempScript -Value $StartScript
$EscapedStartTemp = $TempScript.FullName.Replace("'", "'\''")
$LinuxStartTemp = (& wsl.exe -d $Distro -- bash -lc "wslpath -a '$EscapedStartTemp'").Trim()
Invoke-Wsl "cp $(Quote-Bash $LinuxStartTemp) $(Quote-Bash "$RuntimeRootResolved/scripts/start_windows_wsl2_runtime.sh") && chmod +x $(Quote-Bash "$RuntimeRootResolved/scripts/start_windows_wsl2_runtime.sh")"
Remove-Item $TempScript -Force

$readme = @"
XR Tracking WSL2 Ubuntu 24.04 runtime folder
===========================================

Prepared by:
  devices/xreal_ultra/windows/scripts/wsl2/install_wsl2_ubuntu24_runtime.ps1

This folder is a copied Linux runtime package for Windows+WSL2 mode.
It is not built in WSL2 by this script.

Windows source package copied from:
  $RuntimeSourceRoot

WSL runtime root:
  $RuntimeRootResolved

Start inside WSL2:
  $RuntimeRootResolved/scripts/start_windows_wsl2_runtime.sh

Network/profile config:
  $RuntimeRootResolved/configs/windows_wsl2_network.env

Ubuntu runtime dependencies are installed through:
  devices/xreal_ultra/linux/scripts/install_runtime_deps_ubuntu24.sh --no-groups --no-udev --no-venv
"@
$Temp = New-TemporaryFile
Set-Content -Encoding UTF8 -Path $Temp -Value $readme
$EscapedTemp = $Temp.FullName.Replace("'", "'\''")
$LinuxTemp = (& wsl.exe -d $Distro -- bash -lc "wslpath -a '$EscapedTemp'").Trim()
Invoke-Wsl "cp $(Quote-Bash $LinuxTemp) $(Quote-Bash "$RuntimeRootResolved/README.txt")"
Remove-Item $Temp -Force

Write-Host "[wsl2_runtime] prepared: $Distro:$RuntimeRootResolved"
Write-Host "[wsl2_runtime] copied from: $RuntimeSourceRoot"
Write-Host "[wsl2_runtime] start with: devices\xreal_ultra\windows\scripts\wsl2\start_wsl2_runtime.ps1"
