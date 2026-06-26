param(
  [string]$RegistryPath = "capture_service_streams.json",
  [int]$TcpPort = 45660,
  [int]$CameraIndex = 0,
  [string]$CameraApi = "msmf"
)

$ErrorActionPreference = "Stop"
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$BackendDir = Resolve-Path (Join-Path $ScriptDir "..\..")
$RootProject = Resolve-Path (Join-Path $BackendDir "..")
$Exe = Join-Path $RootProject "bin\capture_service_cpp\capture_service_cpp.exe"
if (-not (Test-Path $Exe)) {
  throw "capture_service_cpp.exe not found: $Exe. Run scripts\windows\build_capture_service_cpp.ps1 first."
}

& $Exe `
  --publish tcp `
  --registry $RegistryPath `
  --namespace xreal_air2ultra_windows `
  --tcp-bind 0.0.0.0 `
  --tcp-port $TcpPort `
  --camera-index $CameraIndex `
  --camera-api $CameraApi
