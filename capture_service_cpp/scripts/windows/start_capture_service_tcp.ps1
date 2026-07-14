param(
  [string]$RegistryPath = "capture_service_streams.json",
  [int]$TcpPort = 45660,
  [int]$CameraIndex = 0,
  [string]$CameraApi = "msmf",
  [string]$Config = "",
  [string]$ConfigDir = "",
  [string]$ConfigName = "config.yaml"
)

$ErrorActionPreference = "Stop"
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$BackendDir = Resolve-Path (Join-Path $ScriptDir "..\..")
$RootProject = Resolve-Path (Join-Path $BackendDir "..")
$Exe = Join-Path $RootProject "bin\capture_service_cpp\capture_service_cpp.exe"
if (-not (Test-Path $Exe)) {
  throw "capture_service_cpp.exe not found: $Exe. Run scripts\windows\build_capture_service_cpp.ps1 first."
}

$Args = @(
  "--publish", "tcp",
  "--registry", $RegistryPath,
  "--tcp-bind", "0.0.0.0",
  "--tcp-port", "$TcpPort"
)
if (-not [string]::IsNullOrWhiteSpace($Config)) {
  $Args += @("--config", $Config)
} elseif (-not [string]::IsNullOrWhiteSpace($ConfigDir)) {
  $Args += @("--config-dir", $ConfigDir, "--config-name", $ConfigName)
} else {
  $Args += @(
    "--namespace", "xreal_air2ultra_windows",
    "--camera-index", "$CameraIndex",
    "--camera-api", $CameraApi
  )
}

& $Exe @Args
