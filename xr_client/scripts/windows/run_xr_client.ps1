param(
  [string]$Root = "",
  [string]$Config = "",
  [switch]$DryRun,
  [switch]$NoStartupGate
)

$ErrorActionPreference = "Stop"

if (-not $Root) {
  $ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
  $Root = Resolve-Path (Join-Path $ScriptDir "..\..\..")
} else {
  $Root = Resolve-Path $Root
}

$env:XR_ROOT_PROJECT = [string]$Root
$env:ROOT_PROJECT = [string]$Root
$env:XR_DEVICE_SCRIPTS_OS = "windows"
$env:XR_DEVICE_HOME = Join-Path $Root "devices\xreal_ultra"
$env:XR_DEVICE_SCRIPTS_ROOT = Join-Path $env:XR_DEVICE_HOME "windows\scripts"
$env:XR_DEVICE_CONFIGS_ROOT = Join-Path $env:XR_DEVICE_HOME "configs"
$env:XR_OUT_ROOT = Join-Path $Root "out\xreal_ultra_windows"
$env:XR_OUT_BIN_ROOT = Join-Path $env:XR_OUT_ROOT "bin"

# Load Windows/WSL2 network profile before Python expands config placeholders
# such as %XR_CAPTURE_TCP_PORT%. In package mode the config lives under
# out/xreal_ultra_windows/devices/xreal_ultra/windows/configs.
$ScriptsRoot = Join-Path $Root "devices\xreal_ultra\windows\scripts"
$EnvScript = Join-Path $ScriptsRoot "xreal_ultra_out_env.ps1"
$NetworkLoader = Join-Path $ScriptsRoot "load_windows_wsl2_network_env.ps1"
if ((Test-Path $EnvScript) -and (Test-Path $NetworkLoader)) {
  . $EnvScript
  . $NetworkLoader
  $null = Import-XrWindowsNetworkEnv -Root ([string]$Root)
}

$PythonCandidates = @(
  (Join-Path $env:XR_OUT_ROOT "bin\python-runtime\venv\Scripts\python.exe"),
  (Join-Path $Root "bin\python-runtime\venv\Scripts\python.exe")
)
$Python = $PythonCandidates | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $Python) {
  $Python = "python"
}

$Client = Join-Path $Root "xr_client\xr_backend_client.py"
$Args = @($Client)
if ($Config) {
  $Args += @("--config", $Config)
} else {
  $DefaultConfig = Join-Path $Root "xr_client\configs\default_windows_tcp.json"
  if (Test-Path $DefaultConfig) {
    $Args += @("--config", $DefaultConfig)
  }
}
if ($DryRun) { $Args += "--dry-run" }
if ($NoStartupGate) { $env:STARTUP_GATE = "0" }

Write-Host "[run_xr_client_windows] Root=$Root"
Write-Host "[run_xr_client_windows] OutRoot=$env:XR_OUT_ROOT"
Write-Host "[run_xr_client_windows] Python=$Python"
& $Python @Args
exit $LASTEXITCODE
