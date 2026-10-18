param(
  [string]$Root = "",
  [string]$Config = "",
  [string]$Device = "",
  [string]$OutRoot = "",
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

$Root = [string]$Root

# Resolve the same JSON profile that Python will use. Device-specific bootstrap
# paths live in windows_launcher in that profile, not in this common launcher.
if (-not $Config) { $Config = "default_windows_tcp" }
$ConfigPath = $Config
if (-not (Test-Path -LiteralPath $ConfigPath -PathType Leaf)) {
  $ConfigFileName = if ($Config.EndsWith(".json")) { $Config } else { "$Config.json" }
  $ConfigDirs = @($env:XR_CLIENT_CONFIG_DIR, (Join-Path $Root "xr_client\configs"), (Join-Path $Root "bin\python\xr_client\configs"))
  foreach ($Directory in $ConfigDirs) {
    if (-not $Directory) { continue }
    $Candidate = Join-Path $Directory $ConfigFileName
    if (Test-Path -LiteralPath $Candidate -PathType Leaf) { $ConfigPath = $Candidate; break }
  }
}
if (-not (Test-Path -LiteralPath $ConfigPath -PathType Leaf)) { throw "xr_client JSON config not found: $Config" }
$ConfigPath = (Resolve-Path -LiteralPath $ConfigPath).Path
$ConfigData = Get-Content -LiteralPath $ConfigPath -Raw -Encoding UTF8 | ConvertFrom-Json
$LauncherConfig = @{}
$LauncherProperty = $ConfigData.PSObject.Properties["windows_launcher"]
if ($LauncherProperty -and $null -ne $LauncherProperty.Value) {
  if ($LauncherProperty.Value -isnot [pscustomobject]) { throw "windows_launcher must be a JSON object" }
  foreach ($Property in $LauncherProperty.Value.PSObject.Properties) { $LauncherConfig[$Property.Name] = $Property.Value }
}

if (-not $Device) { $Device = $env:XR_TARGET_DEVICE }
if (-not $Device) { $Device = $env:XR_DEVICE_TARGET }
if (-not $Device) { $Device = $LauncherConfig["device"] }
if (-not $Device) { $Device = "generic" }
$Device = $Device.ToLowerInvariant().Replace("-", "_")
if ($Device -notmatch '^[a-z0-9][a-z0-9_.]*$') { throw "Invalid device profile: $Device" }

function Set-DefaultEnv([string]$Name, [string]$Value) {
  if (-not [Environment]::GetEnvironmentVariable($Name, "Process")) {
    [Environment]::SetEnvironmentVariable($Name, $Value, "Process")
  }
}

$env:XR_ROOT_PROJECT = $Root
$env:ROOT_PROJECT = $Root
$env:XR_TARGET_DEVICE = $Device
$env:XR_DEVICE_TARGET = $Device
Set-DefaultEnv "XR_DEVICE_SCRIPTS_OS" "windows"
Set-DefaultEnv "XR_DEVICE_HOME" (Join-Path $Root "devices\$Device")
Set-DefaultEnv "XR_DEVICE_SCRIPTS_ROOT" (Join-Path $env:XR_DEVICE_HOME "windows\scripts")
Set-DefaultEnv "XR_DEVICE_CONFIGS_ROOT" (Join-Path $env:XR_DEVICE_HOME "configs")

function Expand-LauncherPath([string]$Value) {
  $Expanded = [Environment]::ExpandEnvironmentVariables($Value.Replace("{root}", $Root).Replace("{device}", $env:XR_DEVICE_HOME).Replace("{device_scripts}", $env:XR_DEVICE_SCRIPTS_ROOT))
  if (-not [IO.Path]::IsPathRooted($Expanded)) { return Join-Path $Root $Expanded }
  return $Expanded
}

if (-not $OutRoot) { $OutRoot = $env:XR_OUT_ROOT }
if (-not $OutRoot) {
  if (Test-Path (Join-Path $Root "bin") -PathType Container) {
    # A deployed package is already the output root.
    $OutRoot = $Root
  } elseif ($LauncherConfig["out_dir"]) {
    $OutRoot = Expand-LauncherPath $LauncherConfig["out_dir"]
  } else {
    $OutRoot = Join-Path $Root "out\xr-gate_windows"
  }
}
if (-not [IO.Path]::IsPathRooted($OutRoot)) { $OutRoot = Join-Path $Root $OutRoot }
$env:XR_OUT_ROOT = [IO.Path]::GetFullPath($OutRoot)
Set-DefaultEnv "XR_OUT_BIN_ROOT" (Join-Path $env:XR_OUT_ROOT "bin")
Set-DefaultEnv "XR_BIN_ROOT" $env:XR_OUT_BIN_ROOT

# Optional profile-provided bootstrap scripts can load device/network settings
# before Python expands placeholders. Existing explicit environment values win.
if ($LauncherConfig["env_script"]) {
  $EnvScript = Expand-LauncherPath $LauncherConfig["env_script"]
  if (-not (Test-Path -LiteralPath $EnvScript -PathType Leaf)) { throw "Windows env script not found: $EnvScript" }
  . $EnvScript
}
if ($LauncherConfig["network_loader"]) {
  $NetworkLoader = Expand-LauncherPath $LauncherConfig["network_loader"]
  if (-not (Test-Path -LiteralPath $NetworkLoader -PathType Leaf)) { throw "Windows network loader not found: $NetworkLoader" }
  . $NetworkLoader
  $null = Import-XrWindowsNetworkEnv -Root $Root
}

$PythonCandidates = @(
  (Join-Path $env:XR_OUT_ROOT "bin\python-runtime\venv\Scripts\python.exe"),
  (Join-Path $Root "bin\python-runtime\venv\Scripts\python.exe")
)
$Python = $env:XR_PYTHON_BIN
if (-not $Python) { $Python = $PythonCandidates | Where-Object { Test-Path $_ } | Select-Object -First 1 }
if (-not $Python) {
  $Python = "python"
}

$Client = Join-Path $Root "xr_client\xr_backend_client.py"
if (-not (Test-Path -LiteralPath $Client -PathType Leaf)) { $Client = Join-Path $Root "bin\python\xr_client\xr_backend_client.py" }
if (-not (Test-Path -LiteralPath $Client -PathType Leaf)) { throw "xr_backend_client.py not found under $Root" }
$ClientArguments = @($Client, "--config", $ConfigPath)
if ($DryRun) { $ClientArguments += "--dry-run" }
if ($NoStartupGate) { $env:STARTUP_GATE = "0" }

Write-Host "[run_xr_client_windows] Root=$Root"
Write-Host "[run_xr_client_windows] OutRoot=$env:XR_OUT_ROOT"
Write-Host "[run_xr_client_windows] Python=$Python"
Write-Host "[run_xr_client_windows] Device=$Device Config=$ConfigPath"
& $Python @ClientArguments
exit $LASTEXITCODE
